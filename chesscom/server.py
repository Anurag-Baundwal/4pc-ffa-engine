"""Plays standard 4-player FFA on chess.com through the variants bot API with ffa_engine.

Polls the API for the bot's current game. Only standard FFA is played (StartFen4 4PC, RuleVariants
"DeadKingWalking EnPassant PromoteTo=D", Anonymous flags allowed); anything else is skipped. On every new
position the game's PGN4 tokens are replayed in the engine ('position startpos pgn4 ...', the audited rules)
and compared with chess.com's FEN; on a disagreement chess.com's FEN is used.

- Our turn: search with the live clock and send the move. Promotions go out as "g7-g8=D"; if chess.com
  doesn't take one within --retry-s, it is resent as "g7-g8=Q".
- Claiming: chess.com's "claim win" is resigning while far enough ahead (the other remaining player gets 20
  for our king and for every zombie king left). After every new ply with two players alive, the engine is
  asked whether resigning leaves us strictly first; if so the server resigns (play=R), even on another
  player's turn.

The bot account (TeamEnigma) is shared with stockfish_4pc and the Chaturaji bot: run only one server at a time.

  python chesscom/server.py
  python chesscom/server.py --replay-dump chesscom/server_dump.txt     (offline: nothing is sent)
"""
import argparse
import io
import json
import os
import re
import sys
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fen4  # noqa: E402
import pgn4  # noqa: E402
from engine_client import EngineClient, EngineError  # noqa: E402
from fen4 import COLORS, COLOR_NAMES  # noqa: E402

if sys.stdout.encoding.lower() != "utf-8":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", line_buffering=True)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
FALLBACK_KEY_DIR = os.path.join(REPO_DIR, "..", "..", "repos6", "stockfish_4pc", "chesscom")


def parse_bool(x):
    return str(x).lower() in ("true", "t", "1", "yes")


def find_seats(names, bot_name):
    """Seats (indices) whose player tag is ours: exact name matches (case-insensitive); if there are none,
    tags starting with bot_name. The caller plays only when exactly one seat matches."""
    key = bot_name.lower()
    exact = [i for i, n in enumerate(names) if n.lower() == key]
    return exact or [i for i, n in enumerate(names) if n.lower().startswith(key)]


def build_parser():
    p = argparse.ArgumentParser(description="ffa_engine on the chess.com bot API (standard FFA)")
    p.add_argument("--prod", type=parse_bool, default=True, help="production server (false = sandbox)")
    p.add_argument("--api-key-file", default=None,
                   help="token file (default chesscom/api_key_prod.txt, else stockfish_4pc's)")
    p.add_argument("--bot-name", default="TeamEnigma1",
                   help="our account: the seat with exactly this name, else the one tag starting with it")
    p.add_argument("--engine", default=os.path.join(REPO_DIR, "ffa_engine.exe"))
    p.add_argument("--hash", type=int, default=256, help="engine hash, MB")
    p.add_argument("--option", action="append", default=[], help="engine option Name=Value (repeatable)")
    p.add_argument("--move-overhead-ms", type=int, default=300,
                   help="time kept back per move (raised to recent poll latency + 150)")
    p.add_argument("--first-move-ms", type=int, default=2500, help="time cap for our first move (anti-abort)")
    p.add_argument("--retry-s", type=float, default=2.0, help="resend a move chess.com hasn't taken after this")
    p.add_argument("--gg", type=parse_bool, default=False, help="say gg in the chat when a game ends")
    p.add_argument("--poll-ms", type=int, default=25)
    p.add_argument("--dump-file", default=os.path.join(SCRIPT_DIR, "server_dump.txt"), help="'' = off")
    p.add_argument("--replay-dump", default=None, help="offline: feed the replies of this dump file")
    p.add_argument("--echo-engine", action="store_true", help="print the commands sent to the engine")
    return p


@dataclass
class Pending:
    engine_move: str
    api_move: str
    sent: datetime
    attempts: int = 1


def engine_move_to_api(move, alternative=False):
    """'h2h3' -> 'h2h3'; promotions 'g7g8d' -> 'g7-g8=D' (alternative: '=Q')."""
    m = re.fullmatch(r"([a-n]\d{1,2})([a-n]\d{1,2})([a-z]?)", move)
    if not m:
        raise ValueError(f"bad engine move {move!r}")
    frm, to, promo = m.groups()
    if promo:
        return f"{frm}-{to}={'Q' if alternative else 'D'}"
    return frm + to


class Server:
    def __init__(self, args, api, engine):
        self.args, self.api, self.engine = args, api, engine
        self.rtts = deque(maxlen=15)
        self.game_nr = None
        self._idle_reply = None
        self.log = print
        self._reset_game()

    def _reset_game(self):
        self.my_color = None
        self.skip_game = False
        self.game_over = False
        self.out_announced = False
        self.claimed = False
        self.pending = {}           # (game number, tokens) -> Pending: our move for that position
        self.claim_checked = set()  # positions checked for a winning resignation
        self.movers = None          # the mover of every PGN token (from the engine)

    # --- per reply -----------------------------------------------------------------------------------------

    def handle(self, reply, now=None):
        if not isinstance(reply, dict) or "pgn4" not in reply or "fen4" not in reply:
            return  # {"info": "no game found"}, {"error": ...}
        now = now or datetime.now(timezone.utc)
        state = (reply["pgn4"], reply["fen4"])
        if state == self._idle_reply:
            return
        if not self._handle(reply, now):
            self._idle_reply = state

    def _handle(self, reply, now):
        """Returns True if this position still needs watching (our move was sent, not yet taken)."""
        game = pgn4.parse_pgn4(reply["pgn4"])
        if game.game_nr != self.game_nr:
            self._new_game(game)
        if self.skip_game:
            return False
        try:
            fen = fen4.parse_fen4(reply["fen4"])
        except ValueError as e:
            self.log(f"[FEN] can't read the FEN ({e}): {reply['fen4']!r}")
            return False
        if fen.game_over:
            if not self.game_over:
                self.game_over = True
                self.log(f"\n[GAME] game {self.game_nr} over: {fen.game_over} | points R/B/Y/G {fen.points}")
                if self.args.gg:
                    self._api_call(self.api.chat, "gg")
            return False
        if self.my_color is None or self.claimed:
            return False
        me = COLORS.index(self.my_color)
        if fen.status[me] != "alive":
            if not self.out_announced:
                self.out_announced = True
                self.log(f"[GAME] we ({COLOR_NAMES[me]}) are out: {fen.status[me]}")
            return False
        key = (self.game_nr, len(game.tokens))

        # claim = resign while that leaves us strictly first; possible on any player's turn
        if fen.status.count("alive") == 2 and key not in self.claim_checked:
            self.claim_checked.add(key)
            self._sync(game, fen, reply["fen4"])
            if self.engine.resign_wins(self.my_color):
                self.log(f"[CLAIM] resigning now leaves us first (points {fen.points}): resigning to claim the win")
                self._api_call(self.api.resign)
                self.claimed = True
                return False

        if fen.turn != self.my_color:
            return False
        p = self.pending.get(key)
        if p:
            # chess.com hasn't taken our move yet (the position is unchanged)
            if p.attempts >= 3 or (now - p.sent).total_seconds() < self.args.retry_s:
                return True
            p.attempts += 1
            p.api_move = engine_move_to_api(p.engine_move, alternative=p.attempts == 2)
            p.sent = now
            self.log(f"[PLAY] {p.engine_move} not taken yet: resending as {p.api_move} (attempt {p.attempts})")
            self._api_call(self.api.play, p.api_move)
            return True
        self._play(game, fen, reply["fen4"], key, now)
        return True

    def _new_game(self, game):
        self.game_nr = game.game_nr
        self._reset_game()
        h = game.headers
        self.log(f"\n[GAME] game {game.game_nr} | {h.get('Variant')} | {h.get('RuleVariants')} | "
                 f"{h.get('TimeControl')} | start {h.get('StartFen4', '')[:20]}")
        if not game.is_standard_ffa():
            self.skip_game = True
            self.log("[GAME] not a standard FFA game: not playing it")
            return
        mine = [COLORS[i] for i in find_seats([game.name(c) for c in COLORS], self.args.bot_name)]
        names = {COLOR_NAMES[i]: game.name(c) for i, c in enumerate(COLORS)}
        if len(mine) != 1:
            self.log(f"[GAME] can't tell our seat (players {names}); not playing")
            self.skip_game = True
            return
        self.my_color = mine[0]
        self.log(f"[GAME] players {names} | we are {COLOR_NAMES[COLORS.index(self.my_color)]}")
        try:
            self.engine.new_game()
        except EngineError as e:
            self.log(f"[ENGINE] {e}; restarting it")
            self.engine.restart()

    # --- engine sync and our move --------------------------------------------------------------------------

    def _sync(self, game, fen, raw_fen):
        """Puts the game into the engine: the PGN replay if it matches chess.com's FEN, else the FEN."""
        movers, error = self.engine.set_pgn4([t.text for t in game.tokens])
        if error:
            self.log(f"[SYNC] the engine couldn't replay the moves ({error}); using chess.com's FEN")
            self.engine.set_fen(fen4.normalize(raw_fen))
            self.movers = None
            return
        diffs = fen4.same_position(fen4.parse_fen4(self.engine.fen()), fen)
        if diffs:
            self.log(f"[SYNC] replay differs from chess.com ({'; '.join(diffs[:6])}); using chess.com's FEN")
            self.engine.set_fen(fen4.normalize(raw_fen))
        self.movers = movers

    def clocks(self, game, now):
        """Everyone's clock in ms (ours counting down live), the increment, and whether we've moved yet."""
        tc = game.time_control() or pgn4.TimeControl(60_000, 5_000, 0)
        clocks = [float(tc.base_ms)] * 4
        moved = False
        if self.movers and len(self.movers) == len(game.tokens):
            for tok, mover in zip(game.tokens, self.movers):
                if mover in COLORS and tok.clock_ms is not None:
                    clocks[COLORS.index(mover)] = tok.clock_ms
                moved |= mover == self.my_color
        me = COLORS.index(self.my_color)
        last_date = game.tokens[-1].date if game.tokens else None
        if last_date is not None:
            elapsed = max(0.0, (now - last_date).total_seconds() * 1000)
            clocks[me] = max(100.0, clocks[me] - max(0.0, elapsed - tc.delay_ms))
        return clocks, tc.inc_ms + int(0.9 * tc.delay_ms), moved

    def _play(self, game, fen, raw_fen, key, now):
        try:
            self._sync(game, fen, raw_fen)
            clocks, inc, moved = self.clocks(game, now)
            me = COLORS.index(self.my_color)
            overhead = max(self.args.move_overhead_ms, (max(self.rtts) + 150) if self.rtts else 0)
            if not moved:
                budget = int(max(150, min(self.args.first_move_ms, clocks[me] / 10) - overhead))
                cmd = f"go movetime {budget}"
            else:
                self.engine.send(f"setoption name MoveOverhead value {overhead}")
                cmd = ("go " + " ".join(f"{c}time {int(clocks[i])}" for i, c in enumerate(COLORS)) + " "
                       + " ".join(f"{c}inc {inc}" for c in COLORS))
            self.log(f"\n[TURN] ply {len(game.tokens)} | clock {clocks[me] / 1000:.1f} s | points R/B/Y/G "
                     f"{fen.points} | {cmd}")
            t0 = time.monotonic()
            best = self.engine.go(cmd, clocks[me] / 1000 + 10)
            if self.engine.stale:
                self.log(f"[ENGINE] dropped stale output before the search: {self.engine.stale}")
            if best and best != "resign":
                best = self._legal_or_retry(best, clocks[me])
            took = int((time.monotonic() - t0) * 1000)
        except EngineError as e:
            self.log(f"[ENGINE] {e}; restarting it")
            self.engine.restart()
            return  # the next poll tries again
        if best == "resign":
            self.log("[CLAIM] the engine resigns to claim the win")
            self._api_call(self.api.resign)
            self.claimed = True
            return
        if not best:
            self.log("[PLAY] the engine returned no move")
            return
        api_move = engine_move_to_api(best)
        reply = self._api_call(self.api.play, api_move)
        self.pending[key] = Pending(best, api_move, now)
        self.log(f"[PLAY] {best} -> {api_move} | {took} ms | reply {reply}")

    def _legal_or_retry(self, best, clock_ms):
        """The engine's move if it's legal here. Otherwise one short new search, and if that is illegal too,
        the first legal move: chess.com would reject an illegal move and we'd lose on time."""
        legal = self.engine.legal_moves()
        if best in legal or not legal:
            return best
        self.log(f"[PLAY] the engine's move {best} is not legal here: searching again")
        budget = int(max(100, min(1000, clock_ms / 20)))
        again = self.engine.go(f"go movetime {budget}", budget / 1000 + 10)
        if again in legal:
            return again
        self.log(f"[PLAY] still not legal ({again}): playing {legal[0]}")
        return legal[0]

    def _api_call(self, fn, *a):
        try:
            return fn(*a)
        except Exception as e:  # ApiError messages are already token-free
            self.log(f"[API] {fn.__name__} failed: {e}")
            return None

    # --- loops ---------------------------------------------------------------------------------------------

    def run(self):
        self.log(f"polling {self.api.url} ...")
        last_logged, last_error = None, None
        while True:
            t0 = time.time()
            try:
                reply = self.api.get_state()
            except Exception as e:
                if str(e) != last_error:
                    self.log(f"[API] poll failed: {e}")
                    last_error = str(e)
                time.sleep(1)
                continue
            last_error = None
            self.rtts.append(int((time.time() - t0) * 1000))
            last_logged = self._dump(reply, last_logged)
            self.handle(reply)
            time.sleep(self.args.poll_ms / 1000)

    def _dump(self, reply, last_logged):
        if not self.args.dump_file:
            return last_logged
        try:
            text = json.dumps(reply)
            with open(self.args.dump_file, "a", encoding="utf-8") as f:
                if text == last_logged:
                    f.write(".")
                else:
                    if last_logged is not None:
                        f.write("\n")
                    f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {text}")
            return text
        except (OSError, TypeError):
            return last_logged


def read_dump(path):
    """(local time, reply) for every entry of a dump file (several entries can share a line)."""
    decoder = json.JSONDecoder()
    with open(path, encoding="utf-8") as f:
        for line in f:
            for m in re.finditer(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\] ", line):
                try:
                    reply, _ = decoder.raw_decode(line, m.end())
                except ValueError:
                    continue
                yield datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S").astimezone(), reply


class FakeApi:
    """Stands in for api.Api offline: records what would have been sent."""
    url = "(offline)"

    def __init__(self):
        self.sent = []

    def play(self, move):
        self.sent.append(("play", move))
        return {"offline": True}

    def resign(self):
        self.sent.append(("resign", "R"))
        return {"offline": True}

    def chat(self, message):
        self.sent.append(("chat", message))
        return {"offline": True}


def engine_options(args):
    opts = {"Hash": str(args.hash)}
    for item in args.option:
        name, _, value = item.partition("=")
        opts[name] = value
    return opts


def find_key_file(args):
    if args.api_key_file:
        return args.api_key_file
    name = "api_key_prod.txt" if args.prod else "api_key_test.txt"
    for d in (SCRIPT_DIR, FALLBACK_KEY_DIR):
        path = os.path.normpath(os.path.join(d, name))
        if os.path.exists(path):
            return path
    raise SystemExit(f"no {name} in {SCRIPT_DIR} or {os.path.normpath(FALLBACK_KEY_DIR)}")


def main():
    args = build_parser().parse_args()
    engine = EngineClient(args.engine, engine_options(args), echo=args.echo_engine)
    if args.replay_dump:
        api = FakeApi()
        server = Server(args, api, engine)
        for when, reply in read_dump(args.replay_dump):
            server.handle(reply, now=when)
        print(f"\n[REPLAY] would have sent: {api.sent}")
        engine.close()
        return

    import api as api_module  # only needed online (requests)
    key_file = find_key_file(args)
    with open(key_file) as f:
        token = f.read().strip()
    print(f"[API] token from {key_file}")
    url = api_module.MAIN_SERVER_URL if args.prod else api_module.TEST_SERVER_URL
    server = Server(args, api_module.Api(url, token, args.bot_name, "v0.1.0"), engine)
    try:
        server.run()
    except KeyboardInterrupt:
        print("\nstopping...")
    finally:
        engine.close()


if __name__ == "__main__":
    main()
