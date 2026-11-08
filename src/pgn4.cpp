/*
  4PC FFA engine - chess.com PGN4 move tokens (see pgn4.h).
*/

#include "pgn4.h"

#include <algorithm>
#include <sstream>

namespace FFA {

std::vector<std::string> tokenize_pgn4(const std::string& text) {
    std::string clean;
    int         depth = 0;
    for (char ch : text) {
        if (ch == '{') depth++;
        else if (ch == '}') depth = std::max(0, depth - 1);
        else if (!depth) clean += ch;
    }
    std::vector<std::string> out;
    std::istringstream       is(clean);
    std::string              t;
    while (is >> t) {
        if (t == "..") continue;
        if (!t.empty() && t.back() == '.' && std::all_of(t.begin(), t.end() - 1, ::isdigit)) continue;
        out.push_back(t);
    }
    return out;
}

Pgn4Token parse_pgn4_token(const std::string& t) {
    Pgn4Token k;
    if (t == "R" || t == "T") {
        k.resignBare = k.ok = true;
        return k;
    }
    if (t == "R#" || t == "T#") {
        k.turnMateBare = k.ok = true;
        return k;
    }
    if (t == "S") {
        k.stalemateBare = k.ok = true;
        return k;
    }
    size_t i      = 0;
    auto   suffix = [&](size_t from) {
        for (size_t j = from; j < t.size(); ++j) {
            char ch = t[j];
            if (ch == '+') k.plus++;
            else if (ch == '#') k.hash++;
            else if (ch == 'S') k.stale++;
            else if (ch == 'R' || ch == 'T') k.resignAfter = true;
            else return false;
        }
        return true;
    };
    if (t.rfind("O-O-O", 0) == 0) {
        k.castle = k.castleLong = true;
        k.ok = suffix(5);
        return k;
    }
    if (t.rfind("O-O", 0) == 0) {
        k.castle = true;
        k.ok = suffix(3);
        return k;
    }
    auto piece_letter = [&]() {
        if (i < t.size() && std::string("KQRBNPD").find(t[i]) != std::string::npos) i++;
    };
    auto square = [&]() -> Square {
        if (i >= t.size() || t[i] < 'a' || t[i] > 'n') return SQ_NONE;
        size_t j = i + 1;
        while (j < t.size() && isdigit((unsigned char)t[j])) j++;
        Square s = string_to_square(t.substr(i, j - i));
        i = j;
        return s;
    };
    piece_letter();
    k.from = square();
    if (k.from == SQ_NONE || i >= t.size() || (t[i] != '-' && t[i] != 'x')) return k;
    i++;
    piece_letter();
    k.to = square();
    if (k.to == SQ_NONE) return k;
    if (i < t.size() && t[i] == '=') {
        k.promotion = true;
        i += 2;
    }
    k.ok = suffix(i);
    return k;
}

bool castle_is_long(const Position& pos, Move m) {
    Color  us = pos.side_to_move();
    Square f = m.from_sq(), t = m.to_sq();
    switch (us) {
    case RED: return file_of(t) < file_of(f);
    case BLUE: return rank_of(t) > rank_of(f);
    case YELLOW: return file_of(t) > file_of(f);
    default: return rank_of(t) < rank_of(f);
    }
}

namespace {
// The players in mask, in turn order after `after`.
std::vector<Color> in_turn_order(uint8_t mask, Color after) {
    std::vector<Color> v;
    Color              c = after;
    for (int i = 0; i < COLOR_NB; ++i) {
        c = next_color(c);
        if (mask & (1 << c)) v.push_back(c);
    }
    return v;
}
}  // namespace

Pgn4Step Pgn4Replay::step(const std::string& t, bool lastToken, bool insufficientEnd, bool resignAfterMove) {
    Pgn4Step  r;
    Pgn4Token k = parse_pgn4_token(t);
    if (!k.ok) {
        r.error = "parse";
        return r;
    }
    Position& pos = game.pos;

    // Markers for things the engine already did when the turn passed: 'S' (stalemate) and 'R#'/'T#'
    // (found mated at the start of the turn).
    if (k.stalemateBare) {
        r.kind = STEP_MARKER;
        if (!pendingStalemates.empty()) {
            r.mover = pendingStalemates.front();
            pendingStalemates.erase(pendingStalemates.begin());
        } else {
            r.mover = pos.side_to_move();
            r.notes.push_back("stalemate-missing");
        }
        return r;
    }
    if (k.turnMateBare) {
        if (!pendingTurnMates.empty()) {
            r.kind  = STEP_MARKER;
            r.mover = pendingTurnMates.front();
            pendingTurnMates.erase(pendingTurnMates.begin());
            return r;
        }
        // Resigning or flagging in check; resign() turns it into a checkmate by the player giving check.
        if (!pos.game_over() && !pos.in_check(pos.side_to_move())) r.notes.push_back("turnmate-missing");
        k.resignBare = true;
    }
    if (!pendingTurnMates.empty()) {
        r.notes.push_back("turnmate-extra");
        pendingTurnMates.clear();
    }
    if (!pendingStalemates.empty()) {
        r.notes.push_back("stalemate-extra");
        pendingStalemates.clear();
    }
    if (pos.game_over()) {
        r.error = "game-over";
        return r;
    }
    const Color mover = pos.side_to_move();
    r.mover = mover;

    if (k.resignBare && lastToken && insufficientEnd) {
        // chess.com ends drawn games on insufficient material with an 'R' token
        pos.end_drawn(END_INSUFFICIENT);
        r.kind = STEP_END;
        return r;
    }
    if (k.resignBare) {
        pos.resign(mover);
        game.history.push_back(pos.board_key());
        r.kind            = STEP_RESIGN;
        r.events          = pos.events();
        pendingStalemates = in_turn_order(r.events.stalemateMask, mover);
        return r;
    }
    if (k.resignAfter && !resignAfterMove) {
        // "Ka7-b7R": the player resigns, then their zombie king makes this move
        pos.resign(mover, false);
        if (pos.game_over() || pos.side_to_move() != mover) {
            r.kind = STEP_RESIGN;
            return r;
        }
    }

    Move list[MAX_MOVES];
    int  n = pos.generate_legal(list);
    Move m = Move::none();
    for (int i = 0; i < n; ++i) {
        if (k.castle) {
            if (list[i].type_of() == CASTLING && castle_is_long(pos, list[i]) == k.castleLong) m = list[i];
        } else if (list[i].from_sq() == k.from && list[i].to_sq() == k.to && list[i].is_promotion() == k.promotion)
            m = list[i];
    }
    if (!m) {
        r.error = "illegal";
        return r;
    }
    game.play(m);
    r.kind            = STEP_MOVE;
    r.move            = m;
    r.events          = pos.events();
    pendingStalemates = in_turn_order(r.events.stalemateMask, mover);
    pendingTurnMates  = in_turn_order(r.events.turnMateMask, mover);
    if (k.stale && !r.events.kingCapMask) {  // an 'S' attached to the move (a king capture is written "#S")
        for (int i = 0; i < k.stale; ++i) {
            if (pendingStalemates.empty()) {
                r.notes.push_back("stalemate-missing");
                break;
            }
            pendingStalemates.erase(pendingStalemates.begin());
        }
    }
    if (k.resignAfter && resignAfterMove && !pos.game_over()) {
        pos.resign(mover);
        game.history.push_back(pos.board_key());
    }
    return r;
}

}  // namespace FFA
