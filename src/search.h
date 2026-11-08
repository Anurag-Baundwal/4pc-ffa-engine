/*
  4PC FFA engine - search: alpha-beta with iterative deepening. Two opponent models (SearchMode):
    brs+:     Best-Reply Search+ (Esser et al. 2013), the default. An opponent layer covers the opponent
              turns before the root player moves again; one of them is searched, the others play their best
              move by move ordering (BMOM), keeping the real turn order and legal positions.
    paranoid: every opponent turn is a MIN ply (all opponents minimize the root player's score).
  Move ordering: the previous iteration's best move first at the root, then captures and promotions by
  MVV-LVA, then quiet moves by history. At depth 0 a root-centric quiescence search resolves captures (see
  qsearch).
*/

#ifndef SEARCH_H_INCLUDED
#define SEARCH_H_INCLUDED

#include <atomic>
#include <cstdint>
#include <string>

#include "position.h"

namespace FFA {

enum SearchMode { MODE_BRS_PLUS, MODE_PARANOID };

struct SearchOptions {
    SearchMode mode         = MODE_BRS_PLUS;
    int        moveOverhead = 30;  // ms kept in reserve per move
    int        qsMaxPly     = 8;   // quiescence plies
    int        qsCheckPly   = 2;   // quiescence plies in which check evasions are searched
};

extern SearchOptions SOpts;

struct SearchLimits {
    int      depth    = 0;
    uint64_t nodes    = 0;
    int      movetime = 0;
    int      time[COLOR_NB] = {0, 0, 0, 0};
    int      inc[COLOR_NB]  = {0, 0, 0, 0};
    bool     infinite = false;
};

struct SearchResult {
    bool     resign = false;  // resigning wins now (chess.com's "claim win")
    Move     best   = Move::none();
    Value    score  = 0;
    int      depth  = 0;
    uint64_t nodes  = 0;
    int      timeMs = 0;
};

// Searches the current position of `game` (the side to move is the root player). Prints UCI info lines
// if `verbose`. Returns when a limit is reached or `stop` becomes true.
SearchResult think(const Game& game, const SearchLimits& limits, std::atomic<bool>& stop, bool verbose);

void search_clear();  // new game: forget the history heuristic

// Writes one line to stdout and flushes it, under a lock. The search thread and the protocol thread both
// print, and std::cout is not thread-safe: two unsynchronized flushes once wrote "bestmove" twice, and the
// spare line was read as the answer to the next "go".
void sync_print(const std::string& line);

const char* mode_name(SearchMode m);
bool        parse_mode(const std::string& s, SearchMode& m);

}  // namespace FFA

#endif  // SEARCH_H_INCLUDED
