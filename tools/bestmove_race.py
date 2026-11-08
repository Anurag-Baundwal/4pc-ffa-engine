"""Regression check for duplicate "bestmove" lines (fixed in uci.cpp/search.cpp: locked, flushed output).

  python tools/bestmove_race.py [ENGINE] [SECONDS]

Sends "go" many times on a position with one legal move (the engine answers at once from its search
thread), each followed by "isready". A second "bestmove" before "readyok" is a duplicate; no answer within
5 s is a lost line. Before the fix the engine wrote ~1 duplicate per 11000 searches; the arena then read the
spare line as the answer to a later "go" and the engine forfeited with an illegal move.
"""
import os
import queue
import subprocess
import sys
import threading
import time

# Arena game (sfmob50, group 15 game 1) just before the duplicate: Yellow's only legal move is g14g13.
POSITION = (
    'position fen R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-1-x,x,x,yR,yN,yB,yK,yQ,yB,yN,yR,x,x,x/x,x,x,yP,yP,yP,'
    'yP,yP,1,yP,yP,x,x,x/x,x,x,8,x,x,x/bR,bP,9,gN,gP,gR/bN,bP,6,yP,3,gP,1/bB,bP,10,gP,gB/bQ,bP,10,gP,gK/b'
    'K,1,bP,9,gP,gQ/bB,bP,10,gP,gB/bN,1,bP,9,gP,1/bR,bP,9,gN,gP,gR/x,x,x,2,rN,3,rP,1,x,x,x/x,x,x,rP,rP,rP'
    ',rP,rP,rP,1,rP,x,x,x/x,x,x,rR,1,rB,rQ,rK,rB,rN,rR,x,x,x moves d2d3 a8d5 k13k12 l11k9 h2h3 d5d8 g13g1'
    '2 k9i10 i2i3 a6b7 j13j11 i10g9 d1e1 d8d7 i14j13 m11l11 i1g3 d7j13 h14m9 g3m9 j13k14 f14h12 h1i2 k14f'
    '9 h12g13 m9k7 a7a8 g13f12 k7f12 f9k14 e13f12 f3d2 k14j14')


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo, "ffa_engine.exe")
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(ln) for ln in p.stdout], daemon=True).start()

    def send(s):
        p.stdin.write(s + "\n")
        p.stdin.flush()

    def wait(prefix, other=None):
        """(found, number of `other` lines before it); found is False after 5 s without it."""
        seen = 0
        while True:
            try:
                line = lines.get(timeout=5)
            except queue.Empty:
                return False, seen
            if line.startswith(prefix):
                return True, seen
            if other and line.startswith(other):
                seen += 1

    send(POSITION)
    dups = lost = n = 0
    t0 = time.time()
    while time.time() - t0 < seconds:
        n += 1
        send("go rtime 3000 btime 3000 ytime 3000 gtime 3000 rinc 40 binc 40 yinc 40 ginc 40")
        ok, _ = wait("bestmove")
        if not ok:
            lost += 1
            send("stop")
        send("isready")
        ok, extra = wait("readyok", "bestmove")
        dups += extra
        lost += not ok
    send("quit")
    print(f"{n} searches in {time.time() - t0:.0f} s: {dups} duplicate bestmoves, {lost} lost lines")
    sys.exit(1 if dups or lost else 0)


if __name__ == "__main__":
    main()
