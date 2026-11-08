#!/usr/bin/env python3
"""Builds an FFA opening book (FEN4 lines) from scraped chess.com games (standard library only).

  python tools/make_book.py --pgns ../4pc-pgn-scraper/pgns/ffa --first 10800 --last 11033 \
      --min-avg 2000 --plies 8 --out books/human2000_ply8.txt

Takes standard FFA games (Variant FFA, start 4PC, DeadKingWalking EnPassant PromoteTo=D) whose four
players average at least --min-avg, keeps the ones whose first --plies plies (whole moves of all four
players) contain no capture, resignation, flag or mate, and writes the position after them, once per
distinct position. The positions follow the humans' choices: a popular move starts many games, and
openings branch so fast that almost every game gives a different position. Each position is replayed
with the engine (position startpos pgn4 ...), which must accept it with all four players alive and no
points scored.
"""
import argparse
import collections
import glob
import os
import re
import statistics
import subprocess

STANDARD = {"Variant": "FFA", "RuleVariants": "DeadKingWalking EnPassant PromoteTo=D"}


def games(paths):
    for fn in paths:
        text = open(fn, encoding="utf-8", errors="replace").read()
        for g in re.split(r"\n\s*\n(?=\[GameNr)", text):
            h = dict(re.findall(r'\[(\w+) "([^"]*)"\]', g))
            if any(h.get(k) != v for k, v in STANDARD.items()) or h.get("StartFen4", "4PC") != "4PC":
                continue
            moves = [re.sub(r"^\d+\.\s*", "", l) for l in g[g.rfind("]") + 1:].strip().split("\n")
                     if re.match(r"^\d+\.", l)]
            yield h, moves


def opening(moves, plies):
    """The first `plies` move tokens, if they are whole rounds of four quiet moves; else None."""
    rounds = plies // 4
    if plies % 4 or len(moves) < rounds:
        return None
    tokens = []
    for line in moves[:rounds]:
        t = line.split(" .. ")
        if len(t) != 4 or any("x" in m or "#" in m or m in ("R", "T", "S") or not m for m in t):
            return None
        tokens += t
    return tokens


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pgns", required=True, help="folder of scraped NNNNN.pgn files")
    ap.add_argument("--first", type=int, required=True, help="first file number")
    ap.add_argument("--last", type=int, required=True, help="last file number")
    ap.add_argument("--min-avg", type=int, default=2000, help="minimum average rating of the four players")
    ap.add_argument("--plies", type=int, default=8)
    ap.add_argument("--engine", default="ffa_engine.exe")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    paths = [p for p in sorted(glob.glob(os.path.join(args.pgns, "*.pgn")))
             if args.first <= int(os.path.basename(p)[:-4]) <= args.last]
    seen, stats = collections.Counter(), collections.Counter()
    picked, avgs = [], []
    for h, moves in games(paths):
        stats["standard"] += 1
        elos = [int(h.get(c + "Elo") or 0) for c in ("Red", "Blue", "Yellow", "Green")]
        if not all(elos) or statistics.mean(elos) < args.min_avg:
            continue
        stats["rated"] += 1
        tokens = opening(moves, args.plies)
        if not tokens:
            continue
        stats["quiet"] += 1
        key = " ".join(tokens)
        seen[key] += 1
        if seen[key] == 1:
            picked.append(tokens)
            avgs.append(statistics.mean(elos))

    eng = subprocess.Popen([os.path.abspath(args.engine)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                           bufsize=1)
    fens, written = [], set()
    for tokens in picked:
        eng.stdin.write("position startpos pgn4 " + " ".join(tokens) + "\nd\ngamestate\n")
        eng.stdin.flush()
        ok = fen = state = None
        while state is None:
            line = eng.stdout.readline().strip()
            if line.startswith("info string pgn4"):
                ok = " ok " in line
            elif line.startswith("Fen: "):
                fen = line[5:]
            elif line.startswith("gamestate"):
                state = line
        if not (ok and fen and "status alive,alive,alive,alive points 0,0,0,0 over 0" in state):
            stats["rejected by engine"] += 1
        elif fen in written:
            stats["transpositions"] += 1  # another move order of a position we have
        else:
            written.add(fen)
            fens.append(fen)
    eng.stdin.write("quit\n")
    eng.stdin.flush()

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# FEN4 after {args.plies} quiet plies of standard chess.com FFA games (files {args.first}-{args.last}), "
                f"average rating >= {args.min_avg}, one line per distinct position (tools/make_book.py)\n")
        for fen in fens:
            f.write(fen + "\n")
    print(f"files {len(paths)}  standard games {stats['standard']}  rated >= {args.min_avg}: {stats['rated']}  "
          f"quiet {args.plies} plies: {stats['quiet']}  distinct: {len(picked)}  "
          f"rejected by engine: {stats['rejected by engine']}  transpositions: {stats['transpositions']}  "
          f"written: {len(fens)}")
    if avgs:
        print(f"average rating: median {statistics.median(avgs):.0f}, max {max(avgs):.0f}; "
              f"openings in more than one game: {sum(1 for v in seen.values() if v > 1)}")


if __name__ == "__main__":
    main()
