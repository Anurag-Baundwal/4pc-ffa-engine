#!/usr/bin/env python3
"""FFA match runner and SPRT for 4-player free-for-all chess engines (standard library only).

Two engines, A (the candidate, --engine1) and B (the base, --engine2), play groups of games. Every group uses
one opening and a list of seat patterns; the default 1-vs-3 patterns ABBB,BABB,BBAB,BBBA put A in every seat
once against three copies of B, so seat advantages cancel within the group. Other patterns (--seating 2v2,
3v1 or --patterns) are possible: any string of four A/B letters per game, seats in turn order R,B,Y,G.

Scoring is pairwise placement: in every game each (A seat, B seat) pair scores 1 for A if A finished with
more points, 0.5 if equal, 0 otherwise. The group's mean is one SPRT sample; the GSPRT uses the normal
approximation with the samples' variance, with bounds given in logistic Elo of that pairwise score (the
Elo by which A's seat would beat a B seat head to head on placement).

A referee process (the base engine binary unless --referee is given) applies every move with the engine's
rules: points, checks, mates, eliminations, zombies and the game end. Players that resign or flag become
zombies whose kings make seeded random legal moves. Engines that exceed their clock flag ("T").

Example:
  python tools/ffa_match.py --engine1 new.exe --engine2 base.exe --tc 4000 --inc 40 --concurrency 4 \\
      --book books/openings.txt --sprt --elo0 0 --elo1 20 --max-groups 2000 --out results.jsonl
"""

import argparse
import collections
import json
import math
import os
import queue
import random
import subprocess
import sys
import threading
import time

COLORS = "rbyg"
START_FEN = "startpos"


# ------------------------------------------------------------------------------------------------
#  Engine process
# ------------------------------------------------------------------------------------------------

class EngineError(Exception):
    pass


class Engine:
    def __init__(self, path, options, label):
        self.label = label
        self.path = path
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        path = os.path.abspath(path)  # Windows can't start "dir/engine.exe" relative paths
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1, creationflags=flags)
        self.lines = queue.Queue()
        self.io = collections.deque(maxlen=40)  # recent I/O, dumped with errors
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.send("uci")
        self.wait_for("uciok", 30)
        for name, value in options.items():
            self.send(f"setoption name {name} value {value}")
        self.sync()

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip("\r\n")
            self.io.append(f"{time.monotonic():.3f} < {line}")
            self.lines.put(line)
        self.lines.put(None)

    def send(self, cmd):
        try:
            self.io.append(f"{time.monotonic():.3f} > {cmd}")
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise EngineError(f"{self.label}: cannot write ({e})")

    def read_line(self, timeout):
        try:
            line = self.lines.get(timeout=max(0.001, timeout))
        except queue.Empty:
            raise TimeoutError
        if line is None:
            raise EngineError(f"{self.label}: process exited")
        return line

    def wait_for(self, prefix, timeout):
        end = time.monotonic() + timeout
        while True:
            line = self.read_line(end - time.monotonic())
            if line.startswith(prefix):
                return line
            if line.startswith("No such option"):
                raise EngineError(f"{self.label}: {line}")

    def sync(self):
        self.send("isready")
        self.wait_for("readyok", 60)

    def position(self, fen, moves):
        cmd = "position startpos" if fen == START_FEN else f"position fen {fen}"
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


class Referee(Engine):
    def state(self, fen, moves):
        self.position(fen, moves)
        self.send("gamestate")
        line = self.wait_for("gamestate", 30).split()
        info = {line[i]: line[i + 1] for i in range(1, len(line) - 1, 2)}
        return {
            "turn": COLORS.index(info["turn"]),
            "status": info["status"].split(","),
            "points": [int(p) for p in info["points"].split(",")],
            "over": info["over"] == "1",
            "reason": info["reason"],
        }

    def legal(self, fen, moves):
        self.position(fen, moves)
        self.send("legalmoves")
        return self.wait_for("legalmoves", 30).split()[1:]


# ------------------------------------------------------------------------------------------------
#  Statistics
# ------------------------------------------------------------------------------------------------

def elo_to_score(elo):
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def score_to_elo(s):
    s = min(max(s, 1e-6), 1 - 1e-6)
    return -400.0 * math.log10(1.0 / s - 1.0)


class Sprt:
    """GSPRT on group scores with the normal approximation. The variance has a floor (0.01, about what
    12 independent pairwise results give) and no decision is taken before min_groups groups, so that a
    few early samples with a tiny spread can't end the test."""

    def __init__(self, elo0, elo1, alpha, beta, min_groups=20):
        self.min_groups = min_groups
        self.s0, self.s1 = elo_to_score(elo0), elo_to_score(elo1)
        self.lower = math.log(beta / (1 - alpha))
        self.upper = math.log((1 - beta) / alpha)

    def llr(self, samples):
        n = len(samples)
        if n < 2:
            return 0.0
        mean = sum(samples) / n
        var = sum((x - mean) ** 2 for x in samples) / n
        var = max(var, 0.01)
        return n * ((mean - self.s0) ** 2 - (mean - self.s1) ** 2) / (2 * var)

    def status(self, samples):
        l = self.llr(samples)
        if len(samples) < self.min_groups:
            return l, None
        return l, ("H1" if l >= self.upper else "H0" if l <= self.lower else None)


def summary(samples, placements):
    n = len(samples)
    mean = sum(samples) / n if n else 0.5
    sd = math.sqrt(sum((x - mean) ** 2 for x in samples) / (n - 1)) if n > 1 else 0.0
    half = 1.96 * sd / math.sqrt(n) if n > 1 else 0.5
    elo = score_to_elo(mean)
    lo, hi = score_to_elo(max(1e-6, mean - half)), score_to_elo(min(1 - 1e-6, mean + half))
    return {"groups": n, "score": mean, "elo": elo, "elo_lo": lo, "elo_hi": hi, "placements": placements}


# ------------------------------------------------------------------------------------------------
#  Games
# ------------------------------------------------------------------------------------------------

def go_command(args, clocks):
    if args.nodes:
        return f"go nodes {args.nodes}"
    if args.depth:
        return f"go depth {args.depth}"
    if args.movetime:
        return f"go movetime {args.movetime}"
    times = " ".join(f"{COLORS[c]}time {max(0, int(clocks[c]))}" for c in range(4))
    incs = " ".join(f"{COLORS[c]}inc {args.inc}" for c in range(4))
    return f"go {times} {incs}"


def search_timeout(args, clock_ms):
    if args.nodes or args.depth:
        return args.fixed_timeout
    if args.movetime:
        return args.movetime / 1000.0 + args.grace
    return clock_ms / 1000.0 + args.grace


def play_game(args, engines, referee, fen, pattern, rng):
    """Plays one game. engines: per seat an Engine. Returns a result dict."""
    moves = []
    clocks = [float(args.tc)] * 4
    errors = []
    traces = []  # engine I/O around each error, and unread output found before a go

    def trace(eng, why):
        traces.append({"engine": eng.label, "ply": len(moves), "why": why, "io": list(eng.io)})

    for e in engines:
        e.send("ucinewgame")
    for e in set(engines):
        e.sync()
    while True:
        st = referee.state(fen, moves)
        if st["over"]:
            break
        if len(moves) >= args.max_plies:
            st["reason"] = "adjudicated_max_plies"
            break
        turn = st["turn"]
        if st["status"][turn] == "zombie":
            legal = referee.legal(fen, moves)
            if not legal:
                errors.append(f"zombie {COLORS[turn]} without moves at ply {len(moves)}")
                st["reason"] = "error"
                break
            moves.append(rng.choice(legal))
            continue
        eng = engines[turn]
        fixed = args.go_override.get(pattern[turn])
        if not eng.lines.empty():
            trace(eng, f"{eng.lines.qsize()} unread lines before go")
        eng.position(fen, moves)
        t0 = time.monotonic()
        eng.send("go " + fixed if fixed else go_command(args, clocks))
        best = None
        try:
            limit = args.fixed_timeout if fixed else search_timeout(args, clocks[turn])
            end = t0 + limit
            while True:
                line = eng.read_line(end - time.monotonic())
                if line.startswith("bestmove"):
                    parts = line.split()
                    best = parts[1] if len(parts) > 1 else None
                    break
                if line.startswith("info string illegal") or line.startswith("info string invalid"):
                    errors.append(f"{eng.label}: {line}")
        except TimeoutError:
            eng.send("stop")
            try:
                eng.wait_for("bestmove", 5)
            except Exception:
                pass
        elapsed = (time.monotonic() - t0) * 1000.0
        if not (args.nodes or args.depth or args.movetime or fixed):
            clocks[turn] -= elapsed
            if clocks[turn] < -args.margin:
                moves.append("T")  # flagged: becomes a zombie (or is mated, if in check)
                continue
            clocks[turn] = max(0.0, clocks[turn]) + args.inc
        if best == "resign":  # resigning while far enough ahead: chess.com's "claim win"
            moves.append("R")
            continue
        if best is None or best in ("(none)", "0000"):
            errors.append(f"{eng.label} gave no move at ply {len(moves)} as {COLORS[turn]}")
            trace(eng, "no move")
            moves.append("T")
            continue
        legal = referee.legal(fen, moves)
        if best not in legal:
            errors.append(f"{eng.label} played illegal {best} at ply {len(moves)} as {COLORS[turn]}")
            trace(eng, "illegal move")
            moves.append("T")
            continue
        moves.append(best)
    return {"pattern": pattern, "fen": fen, "moves": moves, "points": st["points"], "status": st["status"],
            "reason": st["reason"], "errors": errors, **({"traces": traces} if traces else {})}


def pairwise(points, pattern):
    """A's pairwise placement results against B seats in one game."""
    out = []
    for a in range(4):
        if pattern[a] != "A":
            continue
        for b in range(4):
            if pattern[b] != "B":
                continue
            out.append(1.0 if points[a] > points[b] else 0.5 if points[a] == points[b] else 0.0)
    return out


def placement_of(points, seat):
    """1-based place of a seat (ties share the best place)."""
    return 1 + sum(1 for p in points if p > points[seat])


# ------------------------------------------------------------------------------------------------
#  Driver
# ------------------------------------------------------------------------------------------------

SEATINGS = {
    "1v3": "ABBB,BABB,BBAB,BBBA",
    "3v1": "BAAA,ABAA,AABA,AAAB",
    "2v2": "AABB,BAAB,BBAA,ABBA,ABAB,BABA",
}


def parse_options(items):
    opts = {}
    for item in items or []:
        if "=" not in item:
            raise SystemExit(f"bad option {item!r}, expected Name=Value")
        k, v = item.split("=", 1)
        opts[k] = v
    return opts


def load_openings(args):
    if not args.book:
        return [START_FEN]
    with open(args.book, encoding="utf-8") as f:
        fens = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    rng = random.Random(args.seed)
    rng.shuffle(fens)
    return fens


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine1", required=True, help="candidate engine (A)")
    ap.add_argument("--engine2", required=True, help="base engine (B)")
    ap.add_argument("--option1", action="append", help="Name=Value for engine 1 (repeatable)")
    ap.add_argument("--option2", action="append", help="Name=Value for engine 2 (repeatable)")
    ap.add_argument("--referee", help="rules engine (default: engine2)")
    ap.add_argument("--hash", type=int, default=16, help="Hash MB per engine process")
    ap.add_argument("--tc", type=int, default=10000, help="ms per player")
    ap.add_argument("--inc", type=int, default=100, help="ms increment")
    ap.add_argument("--nodes", type=int, default=0)
    ap.add_argument("--depth", type=int, default=0)
    ap.add_argument("--movetime", type=int, default=0)
    ap.add_argument("--go1", help='fixed search limit for engine 1 only, e.g. "nodes 20000" or "depth 3"')
    ap.add_argument("--go2", help="fixed search limit for engine 2 only")
    ap.add_argument("--grace", type=float, default=1.0, help="s of extra time before a search is stopped")
    ap.add_argument("--margin", type=int, default=0, help="ms a player may overrun its clock without flagging")
    ap.add_argument("--fixed-timeout", type=float, default=120.0, help="s allowed per fixed nodes/depth search")
    ap.add_argument("--seating", choices=sorted(SEATINGS), default="1v3")
    ap.add_argument("--patterns", help="comma-separated seat patterns, overrides --seating")
    ap.add_argument("--book", help="FEN4 openings, one per line")
    ap.add_argument("--random-plies", type=int, default=0, help="random legal plies added to each opening")
    ap.add_argument("--max-groups", type=int, default=100)
    ap.add_argument("--book-start", type=int, default=0,
                    help="index of the first opening in the shuffled book (OpenBench gives each runner its own)")
    ap.add_argument("--group-lines", action="store_true",
                    help='print "Finished group N: ..." for every group (for the OpenBench worker)')
    ap.add_argument("--max-plies", type=int, default=1200)
    ap.add_argument("--concurrency", type=int, default=2)
    ap.add_argument("--sprt", action="store_true")
    ap.add_argument("--elo0", type=float, default=0.0)
    ap.add_argument("--elo1", type=float, default=20.0)
    ap.add_argument("--alpha", type=float, default=0.05)
    ap.add_argument("--beta", type=float, default=0.05)
    ap.add_argument("--min-groups", type=int, default=20, help="groups before the SPRT may stop")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", help="jsonl file for every game")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    args.go_override = {"A": args.go1, "B": args.go2}
    patterns = (args.patterns or SEATINGS[args.seating]).split(",")
    for p in patterns:
        if len(p) != 4 or set(p) - set("AB") or "A" not in p or "B" not in p:
            raise SystemExit(f"bad pattern {p!r}")
    opts = {"A": parse_options(args.option1), "B": parse_options(args.option2)}
    for o in opts.values():
        o.setdefault("Hash", str(args.hash))
    paths = {"A": args.engine1, "B": args.engine2}
    openings = load_openings(args)
    sprt = Sprt(args.elo0, args.elo1, args.alpha, args.beta, args.min_groups) if args.sprt else None

    out = open(args.out, "a", encoding="utf-8") if args.out else None
    lock = threading.Lock()
    samples, placements = [], [0, 0, 0, 0]
    reasons = {}
    stop = threading.Event()
    jobs = queue.Queue()
    for g in range(args.max_groups):
        jobs.put(g)
    started = time.monotonic()
    nerrors = [0]

    def report(final=False):
        s = summary(samples, placements)
        line = (f"groups {s['groups']} games {s['groups'] * len(patterns)}  score {s['score']:.4f}  "
                f"elo {s['elo']:+.1f} [{s['elo_lo']:+.1f}, {s['elo_hi']:+.1f}]")
        if len(patterns) == 4 and all(p.count("A") == 1 for p in patterns):
            tot = max(1, sum(placements))
            line += "  A places " + "/".join(f"{100 * x / tot:.0f}" for x in placements) + "%"
        if sprt:
            llr, _ = sprt.status(samples)
            line += f"  LLR {llr:+.2f} ({sprt.lower:.2f}, {sprt.upper:.2f})"
        if nerrors[0]:
            line += f"  errors {nerrors[0]}"
        if final or not args.quiet:
            print(line, flush=True)

    def worker(wid):
        referee = Referee(args.referee or args.engine2, {"Hash": "1"}, f"referee{wid}")
        procs = {}
        try:
            while not stop.is_set():
                try:
                    g = jobs.get_nowait()
                except queue.Empty:
                    break
                fen = openings[(args.book_start + g) % len(openings)]
                rng = random.Random(args.seed * 1000003 + args.book_start + g)
                prefix = []
                for _ in range(args.random_plies):
                    legal = referee.legal(fen, prefix)
                    if not legal:
                        break
                    prefix.append(rng.choice(legal))
                if prefix:
                    referee.position(fen, prefix)
                    referee.send("d")
                    fen = referee.wait_for("Fen:", 30).split(None, 1)[1].strip()
                    referee.wait_for("Key:", 30)
                group = []
                for gi, pattern in enumerate(patterns):
                    seats = []
                    for s in range(4):
                        key = (pattern[s], s)
                        if key not in procs:
                            procs[key] = Engine(paths[pattern[s]], opts[pattern[s]], f"{pattern[s]}{s}.w{wid}")
                        seats.append(procs[key])
                    res = play_game(args, seats, referee, fen, pattern, random.Random(rng.random()))
                    res["group"], res["game"] = g, gi
                    group.append(res)
                    if stop.is_set():
                        break
                if stop.is_set() and len(group) < len(patterns):
                    break
                with lock:
                    pw = [x for r in group for x in pairwise(r["points"], r["pattern"])]
                    samples.append(sum(pw) / len(pw))
                    if args.group_lines:
                        places = [0, 0, 0, 0]
                        for r in group:
                            if r["pattern"].count("A") == 1:
                                places[placement_of(r["points"], r["pattern"].index("A")) - 1] += 1
                        print(f"Finished group {g}: score {samples[-1]:.6f} pairs {pw.count(1.0)} {pw.count(0.5)} "
                              f"{pw.count(0.0)} places {' '.join(map(str, places))} "
                              f"errors {sum(len(r['errors']) for r in group)} "
                              f"{{{','.join(r['reason'] for r in group)}}}", flush=True)
                    for r in group:
                        reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1
                        nerrors[0] += len(r["errors"])
                        for e in r["errors"]:
                            print("  error:", e, file=sys.stderr)
                        if r["pattern"].count("A") == 1:
                            placements[placement_of(r["points"], r["pattern"].index("A")) - 1] += 1
                        if out:
                            out.write(json.dumps(r) + "\n")
                    if out:
                        out.flush()
                    report()
                    if sprt and sprt.status(samples)[1]:
                        stop.set()
        finally:
            referee.close()
            for p in procs.values():
                p.close()

    print(f"A = {args.engine1} {opts['A']}\nB = {args.engine2} {opts['B']}\n"
          f"patterns {','.join(patterns)}  openings {len(openings)}  "
          + (f"nodes {args.nodes}" if args.nodes else f"depth {args.depth}" if args.depth else
             f"movetime {args.movetime}" if args.movetime else f"tc {args.tc}+{args.inc} ms"), flush=True)
    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(args.concurrency)]
    for t in threads:
        t.start()
    try:
        for t in threads:
            while t.is_alive():
                t.join(0.5)
    except KeyboardInterrupt:
        stop.set()
        print("interrupted, finishing current games...", file=sys.stderr)
        for t in threads:
            t.join()
    print("\n=== result ===")
    report(final=True)
    if sprt:
        llr, verdict = sprt.status(samples)
        print(f"SPRT [{args.elo0}, {args.elo1}]: {'H1 accepted (A is stronger)' if verdict == 'H1' else 'H0 accepted' if verdict == 'H0' else 'inconclusive'}")
    print("game ends: " + ", ".join(f"{k} {v}" for k, v in sorted(reasons.items())))
    print(f"time {time.monotonic() - started:.0f} s")
    if out:
        out.close()


if __name__ == "__main__":
    main()
