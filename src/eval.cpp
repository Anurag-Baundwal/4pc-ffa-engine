/*
  4PC FFA engine - hand-crafted evaluation: points and material.
*/

#include "eval.h"

#include <algorithm>
#include <sstream>

namespace FFA {

EvalParams EParams;

int material_of(const Position& pos, Color c) {
    int m = 0;
    for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN}) m += EParams.material[pt] * popcount(pos.pieces(c, pt));
    return m;
}

int standing(const Position& pos, Color c) {
    int s = EParams.pointValue * pos.points(c);
    if (pos.is_alive(c)) s += material_of(pos, c);
    return s;
}

double placement_score(const int points[COLOR_NB], Color c) {
    double s = 0;
    for (int o = 0; o < COLOR_NB; ++o) {
        if (o == c) continue;
        s += points[c] > points[o] ? 1.0 : points[c] == points[o] ? 0.5 : 0.0;
    }
    return s / 3.0;
}

Value terminal_value(const Position& pos, Color root) {
    int pts[COLOR_NB];
    int others = 0;
    for (int c = 0; c < COLOR_NB; ++c) {
        pts[c] = pos.points(Color(c));
        if (c != root) others += pts[c];
    }
    double place = placement_score(pts, root);
    int    diff  = EParams.pointValue * (3 * pts[root] - others) / 3;
    return Value((place - 0.5) * 2 * EParams.resultScale) + std::clamp(diff, -5000, 5000) / 10;
}

Value evaluate(const Position& pos, Color root) {
    if (pos.game_over()) return terminal_value(pos, root);
    Value v = standing(pos, root);
    if (!pos.is_alive(root)) v -= EParams.eliminatedPenalty;
    return v;
}

std::string eval_trace(const Position& pos, Color root) {
    std::ostringstream os;
    for (int c = 0; c < COLOR_NB; ++c)
        os << ColorName[c] << ": points " << pos.points(Color(c)) << " material " << material_of(pos, Color(c))
           << " standing " << standing(pos, Color(c)) << "\n";
    os << "Eval for " << ColorName[root] << ": " << evaluate(pos, root) << "\n";
    return os.str();
}

}  // namespace FFA
