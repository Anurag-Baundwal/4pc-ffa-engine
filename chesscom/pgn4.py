"""PGN4 as the bot API sends it: headers and move tokens with their clock comments.

    [GameNr "110004285"]
    [TimeControl "1+7"]
    [RuleVariants "DeadKingWalking EnPassant PromoteTo=D"]
    1. Ne1-f3 { date=2026-10-11T09:52:41.229Z clock=63490 } .. b9-d9 { ... } ..

"clock" is the mover's remaining time in ms after the move, increment included. The tokens don't say who
made them (eliminated players' slots are dropped); the engine's 'position startpos pgn4 ...' replays them
with the full rules and reports the mover of every token.
"""
import re
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional

COLORS = "rbyg"
COLOR_NAMES = ("Red", "Blue", "Yellow", "Green")
STANDARD_RULES = {"DeadKingWalking", "EnPassant", "PromoteTo=D"}

_TAG_RE = re.compile(r'\[(\w+) "([^"]*)"\]')
_TOKEN_RE = re.compile(r"([^\s{}]+)\s*(?:\{([^}]*)\})?")
_MOVE_NUMBER_RE = re.compile(r"^(\d+\.|\.\.)$")


@dataclass
class Token:
    text: str                       # as in the PGN, e.g. "Bj10xNg7+"
    clock_ms: Optional[int] = None  # the mover's clock after the move
    date: Optional[datetime] = None


@dataclass
class TimeControl:
    base_ms: int
    inc_ms: int
    delay_ms: int


@dataclass
class Game:
    headers: dict
    tokens: list = field(default_factory=list)

    @property
    def game_nr(self):
        return self.headers.get("GameNr")

    def name(self, color):
        return self.headers.get(COLOR_NAMES[COLORS.index(color)], "")

    def time_control(self) -> Optional[TimeControl]:
        m = re.fullmatch(r"([\d.]+)\+([\d.]+)(D?)", self.headers.get("TimeControl", ""))
        if not m:
            return None
        base_ms = int(float(m.group(1)) * 60_000)
        extra_ms = int(float(m.group(2)) * 1000)
        return TimeControl(base_ms, 0 if m.group(3) else extra_ms, extra_ms if m.group(3) else 0)

    def is_standard_ffa(self):
        """Standard FFA: the start position and DeadKingWalking EnPassant PromoteTo=D (Anonymous flags allowed)."""
        rules = set(self.headers.get("RuleVariants", "").split()) - {"Anonymous", "SemiAnonymous"}
        start = "".join(self.headers.get("StartFen4", "4PC").split())
        return self.headers.get("Variant") == "FFA" and rules == STANDARD_RULES and start == "4PC"


def _parse_date(text):
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def parse_pgn4(pgn):
    headers = dict(_TAG_RE.findall(pgn))
    game = Game(headers)
    for token, comment in _TOKEN_RE.findall(_TAG_RE.sub(" ", pgn)):
        if _MOVE_NUMBER_RE.match(token):
            continue
        clock = re.search(r"clock=(\d+)", comment or "")
        date = re.search(r"date=(\S+)", comment or "")
        game.tokens.append(Token(token, int(clock.group(1)) if clock else None,
                                 _parse_date(date.group(1)) if date else None))
    return game
