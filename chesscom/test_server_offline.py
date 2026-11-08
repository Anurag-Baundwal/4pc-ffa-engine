"""Offline tests for the chess.com server (no network: the redaction test only connects to 127.0.0.1).

  python chesscom/test_server_offline.py [--games 6]

1. FEN4 parsing: the two FFA FENs with promoted/dead queens, zombies and dead kings from chess.com, the
   engine's own FENs, and the replies in stockfish_4pc's server dump (Teams games, same FEN4 format).
2. Server replay: fake API replies built from scraped standard FFA games (the PGN4 prefix plus the engine's
   FEN of it); the server must move exactly on its own turns, with legal moves, without syncing errors.
3. Claim: with two players alive and a winning lead, the server resigns (that's chess.com's claim).
4. Promotion retry: a promotion chess.com hasn't taken is resent with =Q.
5. Token redaction: a failed request never shows the token.
6. Seat matching by account name.
7. Illegal-move guard: a stale 'bestmove' line is dropped before the search; an illegal engine move gets one
   new search, then the first legal move.
"""
import argparse
import os
import re
import sys
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fen4  # noqa: E402
import pgn4  # noqa: E402
import server as srv  # noqa: E402
from engine_client import EngineClient  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(REPO, "ffa_engine.exe")
SCRAPED = os.path.join(REPO, "..", "4pc-pgn-scraper", "pgns", "ffa", "10995.pgn")
SF_DUMP = os.path.join(REPO, "..", "..", "repos6", "stockfish_4pc", "chesscom", "server_dump.txt")

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("  FAIL:", what)


USER_FEN_1 = ("B-1,0,0,0-0,1,0,1-1,1,0,1-4,5,5,31-0-\nx,x,x,yR,1,yB,1,yQ,1,yN,yR,x,x,x/\nx,x,x,1,yP,1,yK,2,yP,yP,x,x,x/\n"
              "x,x,x,yP,2,yB,1,yP,2,x,x,x/\nbR,3,bP,2,yN,2,gP,2,gR/\n4,bP,7,gP,1/\nbB,2,bP,2,yP,4,drB,gP,1/\n"
              "bQ,2,bP,bN,6,gP,1,gK/\nbK,1,bP,4,yD,4,gP,1/\nbB,bP,9,gP,1,gB/\nbN,11,gP,1/\nbR,1,bP,drP,1,drP,4,drP,1,gP,gR/\n"
              "x,x,x,4,drP,1,drP,1,x,x,x/\nx,x,x,3,drN,1,drP,1,gN,x,x,x/\nx,x,x,drR,2,drQ,drK,gQ,2,x,x,x")
USER_FEN_2 = ("G-1,0,0,0-0,1,0,0-1,1,0,0-4,20,12,32-0-{'resigned':(null,null,true,null)}-\n"
              "x,x,x,dyR,5,dyN,dyR,x,x,x/\nx,x,x,1,dyP,1,dyB,1,dyD,1,dyP,x,x,x/\nx,x,x,dyP,3,yK,dyP,gP,1,x,x,x/\n"
              "bR,12,gR/\n11,gP,2/\n2,bN,1,bP,1,dyN,4,drB,gP,1/\n1,bQ,1,bP,7,gP,2/\nbK,1,bP,9,gP,gR/\nbB,1,bP,8,gP,1,gK/\n"
              "bN,4,gQ,4,gP,1,gB,1/\nbR,1,bP,drP,1,drP,4,drP,1,gP,1/\nx,x,x,4,drP,1,drP,1,x,x,x/\n"
              "x,x,x,3,drN,1,drP,1,gN,x,x,x/\nx,x,x,drR,2,drQ,drK,3,x,x,x")


def test_fens(engine):
    print("1. FEN4 parsing")
    a = fen4.parse_fen4(USER_FEN_1)
    check(a.status == ["dead", "alive", "alive", "alive"], f"fen 1 status {a.status}")
    check(a.points == [4, 5, 5, 31], f"fen 1 points {a.points}")
    check(a.pieces.get("h7") == ("y", "D", False), f"fen 1 promoted queen {a.pieces.get('h7')}")
    b = fen4.parse_fen4(USER_FEN_2)
    check(b.status == ["dead", "alive", "zombie", "alive"], f"fen 2 status {b.status}")
    check(b.pieces.get("i13") == ("y", "D", True), f"fen 2 dead promoted queen {b.pieces.get('i13')}")
    for name, f in (("fen 1", USER_FEN_1), ("fen 2", USER_FEN_2)):
        engine.set_fen(fen4.normalize(f))
        diffs = fen4.same_position(fen4.parse_fen4(engine.fen()), fen4.parse_fen4(f))
        check(not diffs, f"{name}: engine round trip differs: {diffs}")
    check(fen4.normalize("4PC") == fen4.STANDARD_START_FEN, "4PC shortcut")
    n = bad = 0
    if os.path.exists(SF_DUMP):
        seen = set()
        for _, reply in srv.read_dump(SF_DUMP):
            f = reply.get("fen4") if isinstance(reply, dict) else None
            if not f or f in seen:
                continue
            seen.add(f)
            try:
                fen4.parse_fen4(f)
            except ValueError as e:
                bad += 1
                if bad <= 3:
                    print("   ", e, f[:80])
            n += 1
            if n >= 3000:
                break
        check(bad == 0, f"{bad} of {n} stockfish dump FENs don't parse")
    print(f"   {n} dump FENs parsed")


def scraped_games(limit):
    text = open(SCRAPED, encoding="utf-8", errors="replace").read()
    for g in re.split(r"\n\s*\n(?=\[GameNr)", text):
        game = pgn4.parse_pgn4(g)
        if not game.is_standard_ffa() or game.headers.get("Result", "Aborted") == "Aborted":
            continue
        if "insuff" in game.headers.get("Termination", ""):
            continue
        yield game
        limit -= 1
        if limit <= 0:
            return


def make_reply(game, tokens, seat, engine, start):
    """The API reply after `tokens`: PGN4 with our name in `seat`, fake clocks, and the engine's FEN."""
    headers = dict(game.headers)
    for i, c in enumerate(pgn4.COLOR_NAMES):
        headers[c] = "TeamEnigma1" if i == seat else f"player{i}"
    headers["TimeControl"] = "0.05+0"  # 3 s: fast offline searches
    lines = [f'[{k} "{v}"]' for k, v in headers.items()]
    body = " ".join(f"{t} {{ date={(start + timedelta(seconds=i)).strftime('%Y-%m-%dT%H:%M:%S.000Z')} clock=3000 }}"
                    for i, t in enumerate(tokens))
    movers, err = engine.set_pgn4(tokens)
    return {"pgn4": "\n".join(lines) + "\n" + body, "fen4": engine.fen(), "info": "it's not your turn"}, movers, err


def test_replay(engine, games):
    print("2. server replay on scraped games")
    args = srv.build_parser().parse_args(["--first-move-ms", "100", "--dump-file", ""])
    start = datetime(2026, 11, 7, 12, 0, tzinfo=timezone.utc)
    played = expected = 0
    for gi, game in enumerate(scraped_games(games)):
        seat = gi % 4
        api = srv.FakeApi()
        server = srv.Server(args, api, EngineClient(ENGINE, {"Hash": "16"}))
        server.log = lambda *a: None
        tokens = [t.text for t in game.tokens]
        limit = min(len(tokens), 60)
        for k in range(limit + 1):
            reply, movers, err = make_reply(game, tokens[:k], seat, engine, start)
            check(err is None, f"game {game.game_nr}: replay error {err}")
            state = engine.gamestate()
            ours = (state["turn"] == pgn4.COLORS[seat] and state["status"][seat] == "alive" and not state["over"])
            before = len(api.sent)
            server.handle(reply, now=start + timedelta(seconds=k, milliseconds=200))
            sent = api.sent[before:]
            if ours:
                expected += 1
                if sent and sent[0][0] == "play":
                    played += 1
                    mv = sent[0][1].replace("-", "").split("=")[0]
                    engine.set_pgn4(tokens[:k])
                    engine.send("legalmoves")
                    legal = engine.wait_for("legalmoves", 10).split()[1:]
                    check(any(lm.startswith(mv) for lm in legal), f"game {game.game_nr} ply {k}: illegal {sent[0][1]}")
                elif sent and sent[0][0] == "resign":
                    played += 1
            else:
                check(not [s for s in sent if s[0] == "play"], f"game {game.game_nr} ply {k}: moved on another turn: {sent}")
        server.engine.close()
    check(played == expected, f"moved on {played} of {expected} turns")
    print(f"   moved on {played} of {expected} of our turns")


def test_claim(engine):
    print("3. claim (resign while far enough ahead)")
    args = srv.build_parser().parse_args(["--dump-file", ""])
    base = USER_FEN_2
    headers = ('[GameNr "1"]\n[Variant "FFA"]\n[RuleVariants "DeadKingWalking EnPassant PromoteTo=D"]\n'
               '[StartFen4 "4PC"]\n[TimeControl "1+5"]\n[Red "a"]\n[Blue "b"]\n[Yellow "c"]\n[Green "TeamEnigma1"]\n')
    for pts, should in (("4,20,12,55", False), ("4,20,12,61", True)):
        api = srv.FakeApi()
        server = srv.Server(args, api, engine)
        server.log = lambda *a: None
        server.handle({"pgn4": headers, "fen4": base.replace("4,20,12,32", pts).replace("G-", "B-", 1)},
                      now=datetime.now(timezone.utc))
        resigned = ("resign", "R") in api.sent
        check(resigned == should, f"points {pts}: resigned {resigned}, expected {should}")
    print("   ok" if not FAILS else "")


def test_promotion_retry(engine):
    print("4. promotion retry")
    check(srv.engine_move_to_api("g7g8d") == "g7-g8=D", "promotion format")
    check(srv.engine_move_to_api("g7g8d", alternative=True) == "g7-g8=Q", "alternative promotion format")
    check(srv.engine_move_to_api("h1j1") == "h1j1", "castling format")
    args = srv.build_parser().parse_args(["--dump-file", ""])
    api = srv.FakeApi()
    server = srv.Server(args, api, engine)
    server.log = lambda *a: None
    headers = ('[GameNr "2"]\n[Variant "FFA"]\n[RuleVariants "DeadKingWalking EnPassant PromoteTo=D"]\n'
               '[StartFen4 "4PC"]\n[TimeControl "1+5"]\n[Red "TeamEnigma1"]\n[Blue "b"]\n[Yellow "c"]\n[Green "d"]\n')
    reply = {"pgn4": headers, "fen4": "4PC"}
    t0 = datetime.now(timezone.utc)
    server._new_game(pgn4.parse_pgn4(headers))
    server.game_nr = "2"
    server.pending[("2", 0)] = srv.Pending("g7g8d", "g7-g8=D", t0)
    server.handle(reply, now=t0 + timedelta(seconds=3))
    check(("play", "g7-g8=Q") in api.sent, f"resent as =Q: {api.sent}")


def test_redaction():
    print("5. token redaction")
    import api as api_module
    secret = "SECRET-TOKEN-123"
    a = api_module.Api("http://127.0.0.1:9/bot", secret, "TeamEnigma")
    try:
        a.get_state()
        check(False, "a request to 127.0.0.1:9 should fail")
    except api_module.ApiError as e:
        check(secret not in str(e) and "token=***" in str(e), f"token in error: {e}")
    check(secret not in api_module.redact(f"url ?token={secret}&play=h2h3", secret), "redact()")


def test_seats():
    print("6. seat matching")
    both = ["TeamEnigma1", "anurag_b", "TeamEnigma2", "4pc_observer"]
    check(srv.find_seats(both, "TeamEnigma1") == [0], "exact name with both bots seated")
    check(srv.find_seats(both, "teamenigma2") == [2], "exact name, other case")
    check(len(srv.find_seats(both, "TeamEnigma")) == 2, "ambiguous prefix must not pick a seat")
    check(srv.find_seats(["a", "TeamEnigma1", "b", "c"], "TeamEnigma") == [1], "unique prefix")
    check(srv.find_seats(["a", "b", "c", "d"], "TeamEnigma1") == [], "not seated")
    print("   ok")


def test_illegal_move_guard():
    print("7. stale or illegal engine moves")
    args = srv.build_parser().parse_args(["--first-move-ms", "100", "--dump-file", ""])
    headers = ('[GameNr "3"]\n[Variant "FFA"]\n[RuleVariants "DeadKingWalking EnPassant PromoteTo=D"]\n'
               '[StartFen4 "4PC"]\n[TimeControl "1+5"]\n[Red "TeamEnigma1"]\n[Blue "b"]\n[Yellow "c"]\n[Green "d"]\n')
    reply = {"pgn4": headers, "fen4": "4PC"}
    start_moves = {"d2d3", "e2e3", "f2f3", "g2g3", "h2h3", "i2i3", "j2j3", "k2k3", "d2d4", "e2e4", "f2f4", "g2g4",
                   "h2h4", "i2i4", "j2j4", "k2k4", "e1d3", "e1f3", "j1i3", "j1k3"}

    def run(engine, inject=None, fake_go=None):
        api = srv.FakeApi()
        server = srv.Server(args, api, engine)
        logs = []
        server.log = lambda *a: logs.append(" ".join(map(str, a)))
        real_go = engine.go
        if inject:  # a leftover line arrives just before the search (the sync before it reads older ones)
            engine.go = lambda cmd, t: (engine._lines.put(inject), real_go(cmd, t))[1]
        if fake_go:
            answers = iter(fake_go)
            engine.go = lambda cmd, t: next(answers, None) or real_go(cmd, t)
        server.handle(reply, now=datetime.now(timezone.utc))
        if inject or fake_go:
            del engine.go
        plays = [s[1] for s in api.sent if s[0] == "play"]
        return plays, logs

    engine = EngineClient(ENGINE, {"Hash": "16"})
    plays, logs = run(engine, inject="bestmove a7b7")
    check(len(plays) == 1 and plays[0] in start_moves, f"stale line: played {plays}")
    check(any("stale" in ln and "a7b7" in ln for ln in logs), f"stale line not reported: {logs}")
    plays, logs = run(engine, fake_go=["a7b7"])  # illegal first answer, real search the second time
    check(len(plays) == 1 and plays[0] in start_moves, f"illegal then legal: played {plays}")
    check(any("not legal" in ln for ln in logs), f"illegal move not reported: {logs}")
    plays, logs = run(engine, fake_go=["a7b7", "n7m7"])  # illegal twice: first legal move
    check(plays == ["d2d3"], f"illegal twice: played {plays}, expected the first legal move d2d3")
    engine.close()
    print("   ok" if not [f for f in FAILS if "illegal" in f or "stale" in f] else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", type=int, default=6)
    args = ap.parse_args()
    engine = EngineClient(ENGINE, {"Hash": "16"})
    test_fens(engine)
    test_replay(engine, args.games)
    test_claim(engine)
    test_promotion_retry(engine)
    test_redaction()
    test_seats()
    test_illegal_move_guard()
    engine.close()
    print("\nall passed" if not FAILS else f"\n{len(FAILS)} failures")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
