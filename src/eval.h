/*
  4PC FFA engine - hand-crafted evaluation: points and material.
*/

#ifndef EVAL_H_INCLUDED
#define EVAL_H_INCLUDED

#include <string>

#include "position.h"

namespace FFA {

struct EvalParams {
    int material[PIECE_TYPE_NB] = {0, 100, 300, 350, 500, 950, 0, 0};  // playing value of a piece
    int pointValue              = 100;  // centipawns per point
    int eliminatedPenalty       = 20000;
    int resultScale             = 30000;  // terminal value: (pairwise placement score - 0.5) * 2 * resultScale
};

extern EvalParams EParams;

// Score of the position for player `root`, in centipawns: root's standing, pointValue * points + material
// (material only while root is alive; an eliminated root also gets eliminatedPenalty). The other players'
// standings don't count.
Value evaluate(const Position& pos, Color root);

// Value of a finished game for `root`, from the final placements (ties shared), plus a small points term.
Value terminal_value(const Position& pos, Color root);

// Fraction of the other players that c finished above (ties count half): 1 = first place, 0 = last.
double placement_score(const int points[COLOR_NB], Color c);

int material_of(const Position& pos, Color c);

// Player c's standing: pointValue * points + material (material only while alive).
int standing(const Position& pos, Color c);

std::string eval_trace(const Position& pos, Color root);

}  // namespace FFA

#endif  // EVAL_H_INCLUDED
