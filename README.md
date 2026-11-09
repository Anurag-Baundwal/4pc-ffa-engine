# 4PC FFA engine

A classical engine for chess.com 4-player **free-for-all** on the 14x14 board: alpha-beta search and a
hand-crafted evaluation, built up one tested change at a time (see [Development](#development)). The repo also
contains a rules audit against real chess.com games, an FFA match runner with an SPRT, and a chess.com bot-API
server.

## Layout

| Path | Contents |
|---|---|
| `src/FastUint256.h`, `src/bitboard.*` | 256-bit bitboards on a padded 16x16 grid and slider attacks, taken from stockfish_4pc (`nnue` branch) |
| `src/types.h` | squares, pieces, moves, player states |
| `src/position.*` | FFA rules: move generation, legality, `do_move` with every consequence, FEN4, game end |
| `src/eval.*` | evaluation: the root player's points and material, minus the opponents' mean |
| `src/search.*` | iterative deepening and alpha-beta: BRS+ (default) or paranoid opponent model, quiescence, move ordering |
| `src/uci.*` | UCI-like protocol plus `legalmoves`, `gamestate`, `d`, `resignwins` and PGN4 replay |
| `src/pgn4.*` | chess.com PGN4 move tokens and their replay (shared by the audit and the protocol) |
| `src/audit.cpp` | `ffa_engine audit`: replays chess.com PGN4 games and checks the rules |
| `chesscom/` | bot-API server (`server.py`), API client, FEN4/PGN4 helpers, engine client, offline tests |
| `tools/ffa_match.py` | FFA match runner and SPRT (standard library only) |
| `tools/make_book.py` | builds an opening book from chess.com PGN4 games |
| `books/human2000_ply8.txt` | 31,750 openings: the positions after 8 quiet plies of games whose players average 2000+ |
| `books/human_ply8.txt` | 5,726 openings after 8 plies of human games, no rating filter (older book) |
| `4PC_FFA_RULES.md` | the FFA rules as established by the audit |

## Build

```
make                         # ffa_engine (ffa_engine.exe on Windows), -O3
make EXE=name CXX=clang++    # output name and compiler, as OpenBench workers build it
make debug                   # ffa_engine_debug: assertions on, -O1 -g
```

- Windows: MSYS2 UCRT64 g++ (or clang64 clang++); the exe is linked statically.
- Linux, macOS (Xcode clang) and Android (Termux: `pkg install clang make`) build with the same Makefile.
- x86 builds use `-march=native`, and PEXT where the CPU has BMI2. ARM builds use `-march=armv8-a`.

## Engine commands

```
ffa_engine perft 5                 # start position: 20 395 7800 152050 3452310
ffa_engine perft 4 "<FEN4>"
ffa_engine bench [DEPTH]           # 13 positions, default depth 4; prints Bench: <nodes>
ffa_engine audit pgns/ffa/11000.pgn [--examples 20] [--game ID] [--rule zombieKingAttacks=0]
ffa_engine audit FILES... --openings book.txt --opening-ply 8
ffa_engine                         # protocol on stdin/stdout
```

The protocol follows UCI:

- `position startpos|fen <FEN4> moves ...`, where a move looks like `h2h3`.
  - Promotions look like `g7g8d`.
  - `R`/`T` resign or flag the side to move.
- `position startpos|fen <FEN4> pgn4 <tokens>` replays chess.com PGN4 tokens (`Bj10xNg7+`, `O-O`,
  `Ka7-b7R`, `S`, `R#`, ...) with the audited rules. It answers `info string pgn4 ok movers <rbyg...>`, one
  player per token.
- `go rtime/btime/ytime/gtime/rinc/... | nodes | depth | movetime`. The answer is `bestmove <move>`, or
  `bestmove resign` when resigning now leaves the engine strictly first (chess.com's "claim win").
- Options: `SearchMode` (brs+, paranoid), `MoveOverhead` (ms kept in reserve per move). `Hash` is accepted
  but unused (no transposition table yet).
- Extra commands:
  - `legalmoves`
  - `gamestate`, which prints turn, status, points and whether the game is over
  - `d`, which prints the board, `Fen:` and `Key:`
  - `resignwins [color]`: would resigning now leave that player strictly first?
  - `eval`

## Strength testing

```
python tools/ffa_match.py --engine1 new.exe --engine2 base.exe --tc 4000 --inc 40 \
    --book books/human2000_ply8.txt --concurrency 4 --sprt --elo0 0 --elo1 20 --max-groups 3000 \
    --out results.jsonl
```

- **Groups and seating.** Each opening is played as a group of games, one game per seat pattern.
  - The default `--seating 1v3` (`ABBB,BABB,BBAB,BBBA`) puts the candidate in every seat once against three
    copies of the base engine.
  - `--seating 2v2`, `3v1` or `--patterns ...` give other mixes.
- **Score.** In each game, every (candidate seat, base seat) pair scores 1, ½ or 0 by final points (pairwise
  placement). A group's mean is one SPRT sample.
- **SPRT.** The bounds are in logistic Elo of that pairwise score. The test is a GSPRT with the normal
  approximation; it never stops before `--min-groups` (default 20) groups.
- **Referee.** A referee process (the base binary) applies the rules. Zombies play seeded random legal moves.
  Flagging makes a player a zombie.
- **Search limits.** `--go1 "nodes 20000"` and `--go2 "depth 2"` give each engine its own fixed limit.
- **Engine options.** `--option1 Name=Value` sets an option for one engine.

## chess.com bot server

```
python chesscom/server.py                       # plays standard FFA games of the TeamEnigma1 account
python chesscom/server.py --hash 256
python chesscom/server.py --replay-dump chesscom/server_dump.txt    # offline, sends nothing
python chesscom/test_server_offline.py          # offline tests
```

- **Token.** Read from `chesscom/api_key_prod.txt` (gitignored), else from stockfish_4pc's
  `chesscom/api_key_prod.txt`. Only the path is printed, and errors never show the token.
- **Seat.** `--bot-name` (default TeamEnigma1) is our seat's exact name; if no name matches exactly, the one
  tag starting with it. With two matching tags the server doesn't play.
- **One server at a time.** The account is shared with the stockfish_4pc and Chaturaji servers.
- **Games.** Only standard FFA is played: StartFen4 `4PC` and `DeadKingWalking EnPassant PromoteTo=D`, with
  Anonymous flags allowed. Other games are skipped.
- **Sync.** Each position is replayed in the engine from the PGN4 and compared with chess.com's FEN. On any
  difference, chess.com's FEN wins.
- **Clocks.** Taken from the PGN4 `clock=`/`date=` comments. The first move is capped at 2.5 s against
  aborts.
- **Promotions.** Sent as `g7-g8=D`, then resent as `=Q` if chess.com hasn't taken the move after 2 s.
- **Claims.** Claiming is resigning (`play=R`). After every new ply with two players alive, the server
  resigns if that leaves us strictly first.

## Development

- `master` only receives changes that passed an SPRT, with the result in the commit message, and changes
  that don't alter the search ("No functional change", same bench).
- Every candidate change is one commit on its own branch off `master`. It is tested on OpenBench with the
  branch as Dev and `master` as Base, and merged only if it passes.
- Commit messages end with `Bench: <nodes>`, the output of `ffa_engine bench`.

## License

GPLv3 (see `COPYING`). The bitboard code and the basic types come from stockfish_4pc, which is derived from
Stockfish.
