/*
  4PC FFA engine - chess.com PGN4 move tokens.

  The replay semantics here are the ones established by `ffa_engine audit` (see 4PC_FFA_RULES.md); the audit and the
  protocol's `position ... pgn4 ...` both use Pgn4Replay::step, so they can't drift apart.
*/

#ifndef PGN4_H_INCLUDED
#define PGN4_H_INCLUDED

#include <string>
#include <vector>

#include "position.h"

namespace FFA {

// Move tokens of a PGN4 move text: comments, move numbers and ".." separators removed.
std::vector<std::string> tokenize_pgn4(const std::string& movetext);

struct Pgn4Token {
    bool   resignBare = false;     // "R" / "T": resign or flag instead of moving
    bool   stalemateBare = false;  // "S": the player whose slot this is was stalemated
    bool   turnMateBare = false;   // "R#" / "T#": resigned or flagged while in check (or found mated)
    bool   castle = false, castleLong = false;
    Square from = SQ_NONE, to = SQ_NONE;
    bool   promotion = false;
    int    plus = 0, hash = 0, stale = 0;  // '+', '#', 'S' suffixes
    bool   resignAfter = false;            // "Ka7-b7R": resigned, then the zombie king made this move
    bool   ok = false;
};

// Parses "Qe9xQl9+", "g12xh13=D+", "O-O", "Ka7-b7R", "R", "T", "R#", "S".
Pgn4Token parse_pgn4_token(const std::string& t);

// True if a castling move of the side to move is queenside ("O-O-O").
bool castle_is_long(const Position& pos, Move m);

enum Pgn4StepKind { STEP_MOVE, STEP_RESIGN, STEP_MARKER, STEP_END, STEP_ERROR };

struct Pgn4Step {
    Pgn4StepKind             kind = STEP_ERROR;
    Color                    mover = NO_COLOR;  // the player whose slot the token is
    Move                     move = Move::none();
    MoveEvents               events;            // events of the move (STEP_MOVE)
    std::string              error;             // STEP_ERROR: "parse", "illegal", "game-over"
    std::vector<std::string> notes;             // annotation disagreements, e.g. "stalemate-missing"
};

// Replays tokens one at a time on a Game.
struct Pgn4Replay {
    explicit Pgn4Replay(Game& g) : game(g) {}

    // lastToken + insufficientEnd: the game's Termination says insufficient material and this is the final
    // token (chess.com writes such endings with an 'R'). resignAfterMove: the older reading of "Ka7-b7R"
    // (move first, then resign), kept for experiments.
    Pgn4Step step(const std::string& token, bool lastToken = false, bool insufficientEnd = false,
                  bool resignAfterMove = false);

    Game&              game;
    std::vector<Color> pendingStalemates;  // players the engine stalemated whose 'S' hasn't come yet
    std::vector<Color> pendingTurnMates;   // ... found mated at turn start whose 'R#'/'T#' hasn't come yet
};

}  // namespace FFA

#endif  // PGN4_H_INCLUDED
