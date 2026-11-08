/*
  4PC FFA engine - search (see search.h).
*/

#include "search.h"

#include <algorithm>
#include <chrono>
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

namespace {

using Clock = std::chrono::steady_clock;

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
    // Quiet moves keep their generation order.
    int score_move(const Position& pos, Move m, Move first) const {
        if (m == first) return 100000000;
        if (pos.is_capture(m) || m.is_promotion()) {
            int attacker = EParams.material[type_of(pos.piece_on(m.from_sq()))];
            return 10000000 + victim_gain(pos, m) * 8 - attacker / 8;
        }
        return 0;
    }

    int ordered_moves(const Position& pos, ScoredMove* out, bool capturesOnly, Move first) const {
        Move list[MAX_MOVES];
        int  n = capturesOnly ? pos.generate_captures(list) : pos.generate_legal(list);
        for (int i = 0; i < n; ++i) out[i] = {list[i], score_move(pos, list[i], first)};
        std::stable_sort(out, out + n, [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });
        return n;
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
            // Stand pat: every player may decline to capture. An opponent that declines passes the turn to
            // the next opponent, so the captures of all three opponents are seen; the line ends when the
            // turn would come back to the root player (or at the qsearch ply limit).
            best = stand;
            if (!maxNode && qply + 1 < SOpts.qsMaxPly && ply < MAX_PLY - 2) {
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

    // Alpha-beta, paranoid: the root player's turns maximize, every opponent's turn minimizes. A zombie's
    // turn (chess.com moves its king at random) is a pass. At depth 0 the quiescence search takes over.
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

        ScoredMove moves[MAX_MOVES];
        int        n = ordered_moves(pos, moves, false, ply == 0 ? rootBest : Move::none());
        if (n == 0) return evaluate(pos, root);

        const bool maxNode = us == root;
        Value      best    = maxNode ? -VALUE_INFINITE : VALUE_INFINITE;
        for (int i = 0; i < n; ++i) {
            const Move m     = moves[i].m;
            Position   child = pos;
            child.do_move(m);
            keys.push_back(child.board_key());
            Value v = search(child, depth - 1, ply + 1, alpha, beta);
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
            if (alpha >= beta) break;
        }
        return best;
    }
};

}  // namespace

void search_clear() {}

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
