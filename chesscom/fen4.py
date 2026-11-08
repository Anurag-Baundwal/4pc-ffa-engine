"""chess.com FEN4 for standard 4-player FFA (the bot API's "fen4").

    G-1,0,0,0-0,1,0,0-1,1,0,0-4,20,12,32-0-{'resigned':(null,null,true,null)}-<14 rows>
    0    side to move (R/B/Y/G)
    1    eliminated players R,B,Y,G: 1 = dead (checkmated, stalemated, king captured). A zombie (resigned or
         flagged, king still walking) is NOT marked here but in the dict's 'resigned' / 'flagged'
    2-3  castling rights, kingside / queenside
    4    points R,B,Y,G
    5    plies since the last capture of an alive piece or pawn move
    6    optional dict, Python-like: 'enPassant', 'resigned', 'flagged', 'gameOver', ...
    rows from rank 14 down to rank 1, cells separated by commas: "x" (off the board), a number (empty squares)
    or [d]<color><type>; "d" marks a gray (dead) piece, "D" a promoted queen (worth 1 point). A zombie's
    king has no "d".

At the very start of a game the API may send just "4PC" instead of a FEN.
"""
import re
from dataclasses import dataclass, field

COLORS = "rbyg"
COLOR_NAMES = ("Red", "Blue", "Yellow", "Green")

STANDARD_START_FEN = (
    "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
    "x,x,x,yR,yN,yB,yK,yQ,yB,yN,yR,x,x,x/x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/x,x,x,8,x,x,x/"
    "bR,bP,10,gP,gR/bN,bP,10,gP,gN/bB,bP,10,gP,gB/bQ,bP,10,gP,gK/bK,bP,10,gP,gQ/bB,bP,10,gP,gB/"
    "bN,bP,10,gP,gN/bR,bP,10,gP,gR/x,x,x,8,x,x,x/x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
    "x,x,x,rR,rN,rB,rQ,rK,rB,rN,rR,x,x,x"
)
FILES = "abcdefghijklmn"


@dataclass
class Fen4:
    turn: str                  # 'r', 'b', 'y' or 'g'
    status: list               # per player: 'alive', 'zombie' or 'dead'
    points: list               # 4 ints
    halfmove: int
    extras: dict = field(default_factory=dict)  # gameOver (str), enPassant (4 str), resigned/flagged (4 bools)
    pieces: dict = field(default_factory=dict)  # "e4" -> (color, type, gray); type in PNBRQDK

    @property
    def game_over(self):
        return self.extras.get("gameOver")

    def alive(self, color):
        return self.status[COLORS.index(color)] == "alive"


def normalize(fen):
    """The API wraps the rows with newlines; the engine wants the FEN on one line."""
    fen = "".join(fen.split())
    return STANDARD_START_FEN if fen == "4PC" else fen


def _tuple(text, key):
    m = re.search(r"'%s':\(([^)]*)\)" % key, text)
    return [p.strip().strip("'") for p in m.group(1).split(",")] if m else None


def parse_fen4(fen):
    """Parses a chess.com FEN4. Raises ValueError if it can't be read."""
    fen = normalize(fen)
    extras = {}
    lb, rb = fen.find("{"), fen.rfind("}")
    if lb >= 0:
        if rb < lb:
            raise ValueError("unbalanced {...} in FEN")
        text, fen = fen[lb:rb + 1], fen[:lb] + fen[rb + 1:]
        m = re.search(r"'gameOver':'([^']*)'", text)
        if m:
            extras["gameOver"] = m.group(1)
        for key in ("resigned", "flagged"):
            vals = _tuple(text, key)
            if vals is not None:
                extras[key] = [v == "true" for v in vals][:4]
        eps = _tuple(text, "enPassant")
        if eps is not None:
            extras["enPassant"] = eps[:4]
    parts = [p for p in fen.split("-") if p]
    if len(parts) != 7:
        raise ValueError(f"expected 7 '-'-separated fields, got {len(parts)}")
    turn = parts[0].lower()
    if turn not in COLORS or len(turn) != 1:
        raise ValueError(f"bad side to move {parts[0]!r}")
    try:
        eliminated = [int(v) != 0 for v in parts[1].split(",")]
        points = [int(v) for v in parts[4].split(",")]
        halfmove = int(parts[5])
    except ValueError as e:
        raise ValueError(f"bad number in FEN header: {e}") from None
    if len(eliminated) != 4 or len(points) != 4:
        raise ValueError("eliminated/points fields need 4 values")

    pieces = {}
    rows = parts[6].split("/")
    if len(rows) != 14:
        raise ValueError(f"expected 14 rows, got {len(rows)}")
    gray = [False] * 4
    for r, row in enumerate(rows):
        rank, col = 14 - r, 0
        for cell in row.split(","):
            if cell == "x":
                col += 1
            elif cell.isdigit():
                col += int(cell)
            else:
                dead = len(cell) == 3 and cell[0] == "d"
                body = cell[1:] if dead else cell
                if len(body) != 2 or body[0] not in COLORS or body[1] not in "PNBRQDK":
                    raise ValueError(f"bad piece {cell!r}")
                if col >= 14:
                    raise ValueError(f"row {r} is too long")
                pieces[f"{FILES[col]}{rank}"] = (body[0], body[1], dead)
                gray[COLORS.index(body[0])] |= dead
                col += 1
        if col != 14:
            raise ValueError(f"row {r} has {col} squares")

    status = []
    for i in range(4):
        out = (extras.get("resigned") or [False] * 4)[i] or (extras.get("flagged") or [False] * 4)[i]
        king = any(c == COLORS[i] and t == "K" for c, t, _ in pieces.values())
        if eliminated[i]:
            status.append("dead")
        elif out and king:
            status.append("zombie")
        elif out or gray[i] or not king:
            status.append("dead")
        else:
            status.append("alive")
    return Fen4(turn, status, points, halfmove, extras, pieces)


def same_position(a, b):
    """Compares two parsed FENs on what matters for play: side to move, player states, points and pieces
    (type, owner, promoted queens, gray flags; a zombie's king may or may not carry the gray flag).
    Returns a list of differences (empty if they agree)."""
    diffs = []
    if a.turn != b.turn:
        diffs.append(f"side to move {a.turn} vs {b.turn}")
    if a.status != b.status:
        diffs.append(f"players {a.status} vs {b.status}")
    if a.points != b.points:
        diffs.append(f"points {a.points} vs {b.points}")

    def norm(p, status):
        out = {}
        for sq, (c, t, dead) in p.items():
            zombie_king = t == "K" and status[COLORS.index(c)] == "zombie"
            out[sq] = (c, t, False if zombie_king else dead)
        return out

    pa, pb = norm(a.pieces, a.status), norm(b.pieces, b.status)
    for sq in sorted(set(pa) | set(pb)):
        if pa.get(sq) != pb.get(sq):
            diffs.append(f"{sq}: {pa.get(sq)} vs {pb.get(sq)}")
            if len(diffs) > 12:
                break
    return diffs
