# 4-player chess (chess.com FFA) game rules, as implemented

These are the rules of chess.com 4-player FFA with `RuleVariants "DeadKingWalking EnPassant PromoteTo=D"` and
`StartFen4 "4PC"`. They were established, or corrected, by replaying human games with
`ffa_engine audit` (`src/audit.cpp`) and comparing every move and the final scores with chess.com's.
The switches are in `RuleConfig` (`src/position.h`). `--rule name=value` changes one for an audit run.

**Audit results (2026-11-06), final scores exactly equal to the Result tag:**

| Sample | Standard games | Replayed fully | Exact final scores |
|---|---|---|---|
| `11000.pgn` (used while fitting the rules) | 607 | 605 | 601 (99.3%) |
| 6 files across the archive: 05700, 06500, 07500, 08500, 09500, 10500 | 4,347 | 4,324 | 4,274 (98.8%) |
| `10990`–`10999` | 5,847 | 5,822 | 5,743 (98.6%) |

There are no illegal moves except in a handful of odd games (about 0.2%; see "Unexplained" below). The check
annotations (`+`, `++`, `+++`) agree with chess.com on all but a few moves.

Other checks:
- **Perft** from the start position matches stockfish_4pc (teams) up to depth 6 (20, 395, 7800, 152050,
  3452310, 77430383). Teams and FFA can't differ that early.
- **`audit --verify`** found no problems at any of 452k positions: the incremental hash, the fast
  `has_legal_move` and the FEN4 round trip were all consistent.

## Board and moves
- The board is 14x14 without the 3x3 corners. Turn order is Red, Blue, Yellow, Green. Moves follow standard
  chess, plus 4PC castling and en passant (the stockfish_4pc conventions).
- **Promotion** happens on the relative 8th rank (the centre), to a *promoted queen* (`D`). It moves like a
  queen, but capturing it is worth 1 point.
- **Legality:** a player may not move into check or leave their king in check from any alive player.
- **King capture:** a king can still be captured. This happens when another player's move exposes it (even
  a zombie's move, or the move of a third player) and the attacker moves before the king's owner.
  - The capture is written `xK…#S` and is worth 20 points.
  - The owner becomes dead.

## Player states
- **Alive.**
- **Zombie:** the player resigned (`R`) or flagged (`T`).
  - Their army turns gray: it blocks, can be captured for 0 points, and never attacks.
  - The king keeps moving: it makes random legal king moves, which can include castling. It may capture any
    gray piece, including its own.
  - A zombie king **does attack** the squares around it, so other kings can't step next to it.
  - Checks on a zombie king are annotated but **don't count** for the check bonus.
  - Capturing a zombie king is worth **20 points**.
- **Dead:** checkmated, stalemated, or king captured.
  - The army and king stay on the board as gray pieces, capturable for 0.

## Points
- **Captures:** P 1, N 3, B 5, R 5, Q 9, promoted queen 1, gray pieces 0, zombie king 20.
- **Check bonus:** +1 for a move that newly checks 2 alive kings, +5 for 3.
  - "Newly" means the king is attacked from a square it wasn't attacked from before the move. A piece that
    captures a checking piece on that square counts as a new check.
- **Checkmate:** after every move, any alive or zombie player in check without a legal move is mated
  immediately. The mover gets +20, even if it was another player's piece giving the check.
- **Stalemate:** detected at the start of the player's turn (bare `S`).
  - An alive player who is stalemated gets +20.
  - A stalemated zombie's 20 points are shared, rounded up, by the players alive at that moment (7/7/7 or
    10/10).
- **Resigning or flagging while in check** (`R#`/`T#`): the player giving check gets +20 at once.
  - The resigner still becomes a zombie, but its king is then worth nothing more.
  - A move with `R`/`T` attached (`Ka7-b7R`) means the player resigned and the zombie king made that move.
    So it scores nothing and never triggers this rule.

## Game end
- **Last player alive:** the winner gets +20 for every zombie king still on the board.
- **Claiming the win is resigning** (as you explained; there's no separate claim action, and the bot API
  sends it as `play=R`).
  - With 2 players alive, the other player becomes the last player and gets +20 for the resigner's king
    plus +20 for every zombie king still on the board.
  - So a lead of 21 is enough only when no zombie king is left. With one zombie king you need more than 40,
    and so on. `Position::resign_wins` checks this.
  - The engine always claims when resigning leaves it strictly first: `go` answers `bestmove resign`, and
    the bot server checks after every new ply.
  - The search never assumes an opponent will claim, because a human may play on. An opponent's claim
    would also hand the engine points.
- **Drawn endings:** each of the n alive players gets 20·(n−1)/n (10 for two players, 13 for three, 15 for
  four).
  - **Insufficient material:** 2 alive players with at most one minor piece between them, or 3–4 alive
    players with bare kings. Leftover zombie kings go to the player who made the last move when two are
    alive, and are shared otherwise.
  - **Threefold repetition:** leftover zombie kings are shared.
  - **50-move rule:** 50 moves for every alive player. Zombie moves don't count, and captures of gray pieces
    don't reset the counter. Leftover zombie kings are shared.

## Unexplained (rare)
- In a few older games, a player marked `R` keeps playing normal moves afterwards.
- A few insufficient-material endings come earlier or later than the engine's rule.
- A few games end on chess.com's side with an extra `T`/`R`.
