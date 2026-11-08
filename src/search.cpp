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

    // Alpha-beta, paranoid: the root player's turns maximize, every opponent's turn minimizes. A zombie's
    // turn (chess.com moves its king at random) is a pass. At depth 0 the position is evaluated as it is.
    Value search(const Position& pos, int depth, int ply, Value alpha, Value beta) {
        if (pos.game_over()) return terminal_value(pos, root);
        if (Value cv; claim_value(pos, cv)) return cv;
        if (ply > 0 && is_repetition(pos)) return draw_value(pos);
        nodes++;
        if (check_stop()) return 0;
        if (depth <= 0 || ply >= MAX_PLY - 1) return evaluate(pos, root);

        const Color us = pos.side_to_move();
        if (!pos.is_alive(us)) {
            Position child = pos;
            child.do_pass();
            keys.push_back(child.board_key());
            Value v = search(child, depth, ply + 1, alpha, beta);
            keys.pop_back();
            return v;
        }

        Move moves[MAX_MOVES];
        int  n = pos.generate_legal(moves);
        if (n == 0) return evaluate(pos, root);
        if (ply == 0 && rootBest)
            for (int i = 1; i < n; ++i)
                if (moves[i] == rootBest) {
                    std::swap(moves[0], moves[i]);
                    break;
                }

        const bool maxNode = us == root;
        Value      best    = maxNode ? -VALUE_INFINITE : VALUE_INFINITE;
        for (int i = 0; i < n; ++i) {
            Position child = pos;
            child.do_move(moves[i]);
            keys.push_back(child.board_key());
            Value v = search(child, depth - 1, ply + 1, alpha, beta);
            keys.pop_back();
            if (stopped) return 0;
            if (maxNode ? v > best : v < best) {
                best = v;
                if (ply == 0) {
                    rootBest  = moves[i];
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
