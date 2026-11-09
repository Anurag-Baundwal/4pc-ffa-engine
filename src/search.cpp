/*
  4PC FFA engine - search (see search.h).
*/

#include "search.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include "eval.h"

namespace FFA {

SearchOptions SOpts;

namespace {
std::mutex IoMutex;
}

void sync_print(const std::string& line) {
    std::lock_guard<std::mutex> lock(IoMutex);
    std::cout << line << std::endl;
}

const char* mode_name(SearchMode m) { return m == MODE_PARANOID ? "paranoid" : "brs+"; }

bool parse_mode(const std::string& s, SearchMode& m) {
    if (s == "brs+" || s == "brs") m = MODE_BRS_PLUS;
    else if (s == "paranoid") m = MODE_PARANOID;
    else return false;
    return true;
}

namespace {

using Clock = std::chrono::steady_clock;

// History heuristic: for each player, how often (weighted by depth^2) a quiet move from one square to
// another caused a cutoff. Kept between searches; cleared by search_clear().
int History[COLOR_NB][SQUARE_NB][SQUARE_NB];

struct ScoredMove {
    Move m;
    int  score;
};

struct Searcher {
    Color              root;
    std::atomic<bool>* stopFlag;
    uint64_t           nodes = 0, nodeLimit = 0;
    Clock::time_point  start;
    int64_t            hardMs = 0;
    bool               stopped = false;
    std::vector<Key>   keys;  // board keys of the game so far and of the searched path
    Move               rootBest = Move::none();
    Value              rootScore = 0;

    int64_t elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }

    bool check_stop() {
        if (stopped) return true;
        if ((nodes & 1023) == 0) {
            if (stopFlag->load(std::memory_order_relaxed)) stopped = true;
            if (hardMs > 0 && elapsed() >= hardMs) stopped = true;
        }
        if (nodeLimit && nodes >= nodeLimit) stopped = true;
        return stopped;
    }

    // ---------------------------------------------------------------------------------------------
    //  Move ordering
    // ---------------------------------------------------------------------------------------------

    // What a capture or promotion is worth to the evaluation: the points for the capture plus the material
    // of an alive player's piece (a king capture counts 20000), and the promotion gain.
    int victim_gain(const Position& pos, Move m) const {
        Square to = m.to_sq();
        int    g  = 0;
        if (!pos.empty(to)) {
            Piece v = pos.piece_on(to);
            g += EParams.pointValue * pos.capture_value(to);
            if (pos.is_alive(color_of(v))) g += type_of(v) == KING ? 20000 : EParams.material[type_of(v)];
        }
        if (m.type_of() == EN_PASSANT) g += EParams.pointValue + EParams.material[PAWN];
        if (m.is_promotion()) g += EParams.material[QUEEN] - EParams.material[PAWN];
        return g;
    }

    // The move searched first (the previous iteration's best at the root), then captures and promotions by
    // MVV-LVA: the most valuable victim first and, between equal victims, the least valuable attacker.
    // Then the quiet moves by history.
    int score_move(const Position& pos, Move m, Move first) const {
        if (m == first) return 100000000;
        if (pos.is_capture(m) || m.is_promotion()) {
            int attacker = EParams.material[type_of(pos.piece_on(m.from_sq()))];
            return 10000000 + victim_gain(pos, m) * 8 - attacker / 8;
        }
        return History[pos.side_to_move()][m.from_sq()][m.to_sq()];
    }

    int ordered_moves(const Position& pos, ScoredMove* out, bool capturesOnly, Move first) const {
        Move list[MAX_MOVES];
        int  n = capturesOnly ? pos.generate_captures(list) : pos.generate_legal(list);
        for (int i = 0; i < n; ++i) out[i] = {list[i], score_move(pos, list[i], first)};
        std::stable_sort(out, out + n, [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });
        return n;
    }

    // Best move by move ordering (BMOM), for the opponents that BRS+ doesn't search: the best capture or
    // promotion that doesn't give up more than it wins, else the quiet move with the best history (the first
    // one in generation order on ties).
    Move bmom(const Position& pos) const {
        Move list[MAX_MOVES];
        int  n = pos.generate_legal(list);
        if (n == 0) return Move::none();
        Move bestCap = Move::none(), bestQuiet = Move::none();
        int  bestCapScore = INT32_MIN, bestQuietScore = INT32_MIN;
        for (int i = 0; i < n; ++i) {
            Move m = list[i];
            if (pos.is_capture(m) || m.is_promotion()) {
                int gain     = victim_gain(pos, m);
                int attacker = EParams.material[type_of(pos.piece_on(m.from_sq()))];
                int s        = gain - attacker / 8;
                if (gain + 50 >= attacker && s > bestCapScore) bestCapScore = s, bestCap = m;
            } else {
                int s = History[pos.side_to_move()][m.from_sq()][m.to_sq()];
                if (s > bestQuietScore) bestQuietScore = s, bestQuiet = m;
            }
        }
        return bestCap ? bestCap : bestQuiet ? bestQuiet : list[0];
    }

    // Late move reductions: at depth 3 or more, a quiet move after the first three of its node (or of its
    // player's turn in a BRS+ layer), with the side to move not in check, is searched one ply shallower, or
    // two when its history is zero (it never caused a cutoff) and depth >= 4. The reduced search keeps at
    // least depth 1, and a reduced move that still beats the bound is searched again at full depth.
    int reduction(const Position& pos, Move m, int depth, int index, bool inCheck) const {
        if (depth < 3 || index < 3 || inCheck || pos.is_capture(m) || m.is_promotion()) return 0;
        int r = 1 + (History[pos.side_to_move()][m.from_sq()][m.to_sq()] == 0 && depth >= 4);
        return std::min(r, depth - 2);
    }

    // A quiet move caused a cutoff: its history grows by depth^2 (all history is halved when an entry
    // passes 1,000,000).
    void update_quiet(const Position& pos, Move m, int depth) {
        if (pos.is_capture(m) || m.is_promotion()) return;
        int& h = History[pos.side_to_move()][m.from_sq()][m.to_sq()];
        h += depth * depth;
        if (h > 1000000)
            for (auto& a : History)
                for (auto& b : a)
                    for (auto& x : b) x /= 2;
    }

    // ---------------------------------------------------------------------------------------------
    //  Helpers
    // ---------------------------------------------------------------------------------------------

    bool is_repetition(const Position& pos) const {
        Key k = pos.board_key();
        int limit = std::min<int>(int(keys.size()) - 1, pos.rule50_count() + 1);
        for (int i = 2; i <= limit; ++i)
            if (keys[keys.size() - 1 - i] == k) return true;
        return false;
    }

    Value draw_value(const Position& pos) const {
        Position p = pos;
        p.end_drawn(END_REPETITION);
        return terminal_value(p, root);
    }

    // The root player resigns ("claims the win") as soon as resigning leaves it strictly first. Opponents
    // are never assumed to: a human may well play on, and their claim would only hand the root points.
    bool claim_value(const Position& pos, Value& v) const {
        if (pos.alive_count() != 2 || !pos.is_alive(root) || !pos.resign_wins(root)) return false;
        Position p = pos;
        p.resign(root);
        v = terminal_value(p, root);
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    //  Quiescence: one player per ply, captures (and promotions); evasions near the horizon.
    // ---------------------------------------------------------------------------------------------

    Value qsearch(const Position& pos, int ply, Value alpha, Value beta, int qply) {
        nodes++;
        if (check_stop()) return 0;
        if (pos.game_over()) return terminal_value(pos, root);
        if (Value cv; claim_value(pos, cv)) return cv;
        const Color us = pos.side_to_move();
        if (!pos.is_alive(us)) {  // a zombie's turn: a pass
            if (qply >= SOpts.qsMaxPly) return evaluate(pos, root);
            Position child = pos;
            child.do_pass();
            return qsearch(child, ply + 1, alpha, beta, qply + 1);
        }
        const bool maxNode = us == root;
        const bool inCheck = pos.in_check(us) && qply < SOpts.qsCheckPly;
        Value      stand   = evaluate(pos, root);
        Value      best;
        if (!inCheck) {
            // Stand pat: every player may decline to capture. In the first qsPassPly plies an opponent that
            // declines passes the turn to the next opponent, so the captures of all three opponents are seen
            // right after the horizon; the chain ends when the turn would come back to the root player.
            // Later, a declining opponent ends the line.
            best = stand;
            if (!maxNode && qply < SOpts.qsPassPly && qply + 1 < SOpts.qsMaxPly && ply < MAX_PLY - 2) {
                Position child = pos;
                child.do_pass();
                while (!child.game_over() && child.side_to_move() != root && !child.is_alive(child.side_to_move()))
                    child.do_pass();  // zombies' turns are passes too
                if (!child.game_over() && child.side_to_move() != root) {
                    best = qsearch(child, ply + 1, alpha, beta, qply + 1);
                    if (stopped) return 0;
                }
            }
            if (maxNode) {
                if (best >= beta) return best;
                alpha = std::max(alpha, best);
            } else {
                if (best <= alpha) return best;
                beta = std::min(beta, best);
            }
            if (qply >= SOpts.qsMaxPly || ply >= MAX_PLY - 1) return best;
        } else
            best = maxNode ? -VALUE_INFINITE : VALUE_INFINITE;

        ScoredMove moves[MAX_MOVES];
        int        n = ordered_moves(pos, moves, !inCheck, Move::none());
        if (n == 0) return inCheck ? stand : best;
        for (int i = 0; i < n; ++i) {
            const Move m = moves[i].m;
            if (!inCheck) {
                // Root-centric quiescence: the root player's captures, and the opponents' captures of the
                // root player's pieces (or of a king). Opponents trading among themselves barely changes
                // the root's score. Delta pruning on top.
                Square to      = m.to_sq();
                Piece  victim  = pos.piece_on(to);
                bool   kingCap = victim != NO_PIECE && type_of(victim) == KING;
                int    gain    = victim_gain(pos, m);
                if (maxNode) {
                    if (!kingCap && stand + gain + 200 <= alpha) continue;
                } else {
                    bool hitsRoot = victim != NO_PIECE && color_of(victim) == root;
                    if (m.type_of() == EN_PASSANT)
                        for (int c = 0; c < COLOR_NB; ++c)
                            if (pos.ep_square(Color(c)) == to && c == root) hitsRoot = true;
                    if (!hitsRoot && !kingCap) continue;
                    if (!kingCap && stand - gain - 200 >= beta) continue;
                }
            }
            Position child = pos;
            child.do_move(m);
            Value v = qsearch(child, ply + 1, alpha, beta, qply + 1);
            if (stopped) return 0;
            if (maxNode) {
                if (v > best) best = v;
                if (v > alpha) alpha = v;
            } else {
                if (v < best) best = v;
                if (v < beta) beta = v;
            }
            if (alpha >= beta) break;
        }
        return best;
    }

    // The next node after a move: in BRS+ an opponent's turn starts an opponent layer.
    Value next_node(const Position& child, int depth, int ply, Value alpha, Value beta) {
        if (SOpts.mode == MODE_BRS_PLUS && !child.game_over() && child.side_to_move() != root)
            return brs_layer(child, depth, ply, alpha, beta);
        return search(child, depth, ply, alpha, beta);
    }

    // Alpha-beta. In paranoid mode every ply is one player's turn: the root player's turns maximize, every
    // opponent's turn minimizes. In BRS+ mode this is only called with the root player to move; the
    // opponents' turns are handled by brs_layer(). A zombie's turn (chess.com moves its king at random) is
    // a pass. At depth 0 the quiescence search takes over.
    Value search(const Position& pos, int depth, int ply, Value alpha, Value beta) {
        if (pos.game_over()) return terminal_value(pos, root);
        if (Value cv; claim_value(pos, cv)) return cv;
        if (ply > 0 && is_repetition(pos)) return draw_value(pos);
        if (depth <= 0 || ply >= MAX_PLY - 1) return qsearch(pos, ply, alpha, beta, 0);
        nodes++;
        if (check_stop()) return 0;

        const Color us = pos.side_to_move();
        if (!pos.is_alive(us)) {
            Position child = pos;
            child.do_pass();
            keys.push_back(child.board_key());
            Value v = search(child, depth, ply + 1, alpha, beta);
            keys.pop_back();
            return v;
        }
        if (SOpts.mode == MODE_BRS_PLUS && us != root) return brs_layer(pos, depth, ply, alpha, beta);

        ScoredMove moves[MAX_MOVES];
        int        n = ordered_moves(pos, moves, false, ply == 0 ? rootBest : Move::none());
        if (n == 0) return evaluate(pos, root);

        const bool maxNode = us == root;
        const bool inCheck = pos.in_check(us);
        Value      best    = maxNode ? -VALUE_INFINITE : VALUE_INFINITE;
        for (int i = 0; i < n; ++i) {
            const Move m     = moves[i].m;
            Position   child = pos;
            child.do_move(m);
            keys.push_back(child.board_key());
            const int r = reduction(pos, m, depth, i, inCheck);
            Value     v = next_node(child, depth - 1 - r, ply + 1, alpha, beta);
            if (r > 0 && !stopped && (maxNode ? v > alpha : v < beta)) v = next_node(child, depth - 1, ply + 1, alpha, beta);
            keys.pop_back();
            if (stopped) return 0;
            if (maxNode ? v > best : v < best) {
                best = v;
                if (ply == 0) {
                    rootBest  = m;
                    rootScore = v;
                }
            }
            if (maxNode)
                alpha = std::max(alpha, v);
            else
                beta = std::min(beta, v);
            if (alpha >= beta) {
                update_quiet(pos, m, depth);
                break;
            }
        }
        return best;
    }

    // BRS+ opponent layer (Esser et al. 2013): pos has the first opponent to move. Every opponent's moves
    // are tried in turn while the other opponents of the layer play their BMOM, keeping the real turn order
    // and legal positions; the layer is a MIN node over all those lines and ends when the root player is to
    // move again. It counts as one depth unit.
    Value brs_layer(const Position& pos, int depth, int ply, Value alpha, Value beta) {
        if (pos.game_over()) return terminal_value(pos, root);
        if (Value cv; claim_value(pos, cv)) return cv;
        if (depth <= 0 || ply >= MAX_PLY - 2) return qsearch(pos, ply, alpha, beta, 0);
        nodes++;
        if (check_stop()) return 0;

        Value    best = VALUE_INFINITE;
        Position cur  = pos;  // the position before the current opponent's turn
        for (int turn = 0; turn < 4 && !cur.game_over() && cur.side_to_move() != root; ++turn) {
            if (!cur.is_alive(cur.side_to_move())) {
                cur.do_pass();
                continue;
            }
            ScoredMove moves[MAX_MOVES];
            int        n       = ordered_moves(cur, moves, false, Move::none());
            const bool inCheck = cur.in_check(cur.side_to_move());
            for (int i = 0; i < n; ++i) {
                Position child = cur;
                child.do_move(moves[i].m);
                // the remaining opponents of this layer play their BMOM
                for (int k = 0; k < 4 && !child.game_over() && child.side_to_move() != root; ++k) {
                    if (!child.is_alive(child.side_to_move()))
                        child.do_pass();
                    else
                        child.do_move(bmom(child));
                }
                keys.push_back(child.board_key());
                const int r = reduction(cur, moves[i].m, depth, i, inCheck);
                Value     v = search(child, depth - 1 - r, ply + 1, alpha, beta);
                if (r > 0 && !stopped && v < beta) v = search(child, depth - 1, ply + 1, alpha, beta);
                keys.pop_back();
                if (stopped) return 0;
                best = std::min(best, v);
                beta = std::min(beta, v);
                if (alpha >= beta) {
                    update_quiet(cur, moves[i].m, depth);
                    return best;
                }
            }
            Move b = bmom(cur);
            if (!b) break;
            cur.do_move(b);
        }
        if (best == VALUE_INFINITE) {  // no opponent could move: continue with the root player's turn
            keys.push_back(cur.board_key());
            Value v = search(cur, depth - 1, ply + 1, alpha, beta);
            keys.pop_back();
            return v;
        }
        return best;
    }
};

}  // namespace

void search_clear() { std::memset(History, 0, sizeof(History)); }

SearchResult think(const Game& game, const SearchLimits& limits, std::atomic<bool>& stop, bool verbose) {
    SearchResult    res;
    const Position& pos = game.pos;
    if (pos.game_over()) return res;

    auto s      = std::make_unique<Searcher>();
    s->root     = pos.side_to_move();
    s->stopFlag = &stop;
    s->start    = Clock::now();
    s->keys     = game.history;

    if (pos.resign_wins(s->root)) {
        res.resign = true;
        return res;
    }
    Move list[MAX_MOVES];
    int  n = pos.generate_legal(list);
    if (n == 0) return res;
    res.best = list[0];

    // Time: a soft limit to stop iterating and a hard limit to abort. An aborted iteration is thrown away.
    int64_t softMs = 0;
    s->nodeLimit   = limits.nodes;
    if (limits.movetime > 0) {
        s->hardMs = std::max<int64_t>(1, limits.movetime - SOpts.moveOverhead);
        softMs    = s->hardMs;
    } else if (!limits.infinite && limits.time[s->root] > 0) {
        int64_t t   = limits.time[s->root];
        int64_t inc = limits.inc[s->root];
        int64_t lim = std::max<int64_t>(1, t - SOpts.moveOverhead);
        softMs      = std::min<int64_t>(t / 30 + inc * 3 / 4, lim / 4);
        s->hardMs   = std::min<int64_t>(softMs * 3, lim / 3);
        softMs      = std::max<int64_t>(softMs, 1);
        s->hardMs   = std::max<int64_t>(s->hardMs, 1);
    }
    int maxDepth = limits.depth > 0 ? std::min(limits.depth, MAX_PLY - 2) : MAX_PLY - 2;

    if (n == 1 && limits.depth == 0 && limits.nodes == 0 && !limits.infinite) {
        res.best  = list[0];
        res.score = evaluate(pos, s->root);
        if (verbose)
            sync_print("info depth 0 score cp " + std::to_string(res.score) + " nodes 0 pv " + move_to_string(list[0]));
        return res;
    }

    for (int depth = 1; depth <= maxDepth; ++depth) {
        Value v = s->search(pos, depth, 0, -VALUE_INFINITE, VALUE_INFINITE);
        if (s->stopped && depth > 1) break;
        res.best   = s->rootBest ? s->rootBest : res.best;
        res.score  = v;
        res.depth  = depth;
        res.nodes  = s->nodes;
        res.timeMs = int(s->elapsed());
        if (verbose) {
            int64_t            ms = std::max<int64_t>(1, s->elapsed());
            std::ostringstream os;
            os << "info depth " << depth << " score cp " << v << " nodes " << s->nodes << " nps "
               << s->nodes * 1000 / ms << " time " << s->elapsed() << " pv " << move_to_string(res.best);
            sync_print(os.str());
        }
        if (s->stopped) break;
        if (softMs > 0 && s->elapsed() * 2 > softMs) break;
    }
    res.nodes  = s->nodes;
    res.timeMs = int(s->elapsed());
    return res;
}

}  // namespace FFA
