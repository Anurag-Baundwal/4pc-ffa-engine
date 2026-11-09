/*
  4PC FFA engine - UCI-like protocol.

  Standard commands: uci, isready, ucinewgame, setoption, position, go, stop, quit.
    position [startpos | fen <FEN4>] [moves <m1> <m2> ... | pgn4 <t1> <t2> ...]
      moves are "d2d4", promotions "g7g8d"; "R" (resign) / "T" (flag) make the side to move a zombie.
      pgn4 takes chess.com PGN4 move tokens ("Bj10xNg7+", "O-O", "Ka7-b7R", "S", "R#", ...) and replays them
      with the audit's rules, then prints "info string pgn4 ok movers <rbyg...>" (one player per token) or
      "info string pgn4 error <what> at <index> token <t>".
    go [depth N] [nodes N] [movetime MS] [rtime MS btime MS ytime MS gtime MS] [rinc MS ...] [infinite]
    go perft N    (prints the move counts per root move and the total)
  Extensions for match runners:
    legalmoves    -> "legalmoves m1 m2 ..."
    gamestate     -> "gamestate turn <c> status <s,s,s,s> points <p,p,p,p> over <0|1> reason <r>"
    d             -> board, then "Fen: <FEN4>" and "Key: <hex>"
    eval          -> evaluation breakdown for the side to move
    resignwins [c]-> "resignwins yes|no": would resigning now leave c (default: side to move) strictly first?
  go answers "bestmove resign" when resigning wins (chess.com's "claim win"). If the engine thinks the game
  is over but is asked to move anyway (chess.com may disagree), it searches on as if it weren't.
*/

#include "uci.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <functional>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
    #include <pthread.h>
#endif

#include "eval.h"
#include "pgn4.h"
#include "search.h"
#include "tt.h"

namespace FFA {

namespace {

// The search thread. The search recurses with a Position (~1.2 KB) and a move list per ply, about 0.5 MB at
// MAX_PLY, but std::thread can't choose a stack size: threads get 512 KB on macOS, ~1 MB on Android and
// 128 KB with musl. Outside Windows the thread is a pthread with a 16 MB stack. On Windows std::thread
// already gets the 16 MB that the Makefile reserves in the exe.
class NativeThread {
public:
    void start(std::function<void()> f) {
#if !defined(_WIN32)
        fn_ = std::move(f);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, size_t(16) << 20);
        const bool ok = pthread_create(&native_, &attr, entry, this) == 0;
        pthread_attr_destroy(&attr);
        if (ok) {
            nativeRunning_ = true;
            return;
        }
        f = std::move(fn_);  // couldn't get the big stack: run with the default one
#endif
        thread_ = std::thread(std::move(f));
    }
    bool joinable() const { return nativeRunning_ || thread_.joinable(); }
    void join() {
#if !defined(_WIN32)
        if (nativeRunning_) {
            pthread_join(native_, nullptr);
            nativeRunning_ = false;
            return;
        }
#endif
        thread_.join();
    }

private:
#if !defined(_WIN32)
    static void* entry(void* self) {
        static_cast<NativeThread*>(self)->fn_();
        return nullptr;
    }
    pthread_t             native_{};
    std::function<void()> fn_;
#endif
    bool        nativeRunning_ = false;
    std::thread thread_;
};

Game              G;
NativeThread      SearchThread;
std::atomic<bool> StopFlag{false};
bool              Searching = false;
int               HashMB = 16;

void wait_search() {
    if (SearchThread.joinable()) SearchThread.join();
    Searching = false;
}

const char* status_name(PlayerStatus s) { return s == ALIVE ? "alive" : s == ZOMBIE ? "zombie" : "dead"; }

}  // namespace

uint64_t perft(const Position& pos, int depth) {
    Move list[MAX_MOVES];
    if (pos.game_over()) return 0;
    if (!pos.is_alive(pos.side_to_move()) && pos.status(pos.side_to_move()) == DEAD) return 0;
    int n = pos.generate_legal(list);
    if (depth <= 1) return depth == 1 ? uint64_t(n) : 1;
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
        Position child = pos;
        child.do_move(list[i]);
        total += perft(child, depth - 1);
    }
    return total;
}

void perft_divide(const Position& pos, int depth) {
    auto     t0 = std::chrono::steady_clock::now();
    Move     list[MAX_MOVES];
    int      n = pos.generate_legal(list);
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
        Position child = pos;
        child.do_move(list[i]);
        uint64_t c = depth > 1 ? perft(child, depth - 1) : 1;
        total += c;
        std::cout << move_to_string(list[i]) << ": " << c << "\n";
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\nNodes searched: " << total << "\nTime: " << ms << " ms\n" << std::flush;
}

bool apply_token(Game& game, const std::string& token) {
    if (game.pos.game_over()) return false;
    if (token == "R" || token == "T" || token == "resign" || token == "timeout") {
        game.pos.resign(game.pos.side_to_move());
        game.history.push_back(game.pos.board_key());
        return true;
    }
    if (token == "pass" || token == "0000") {
        if (game.pos.is_alive(game.pos.side_to_move())) return false;
        game.pass_turn();
        return true;
    }
    Move m = game.pos.parse_move(token);
    if (!m) return false;
    game.play(m);
    return true;
}

std::string game_state_line(const Position& pos) {
    std::ostringstream os;
    os << "gamestate turn " << ColorChar[pos.side_to_move()] << " status ";
    for (int c = 0; c < COLOR_NB; ++c) os << status_name(pos.status(Color(c))) << (c < 3 ? "," : "");
    os << " points ";
    for (int c = 0; c < COLOR_NB; ++c) os << pos.points(Color(c)) << (c < 3 ? "," : "");
    os << " over " << (pos.game_over() ? 1 : 0) << " reason " << end_reason_name(pos.end_reason());
    return os.str();
}

static void cmd_position(std::istringstream& is) {
    std::string tok, fen, mode;
    is >> tok;
    Position p;
    if (tok == "startpos") {
        p.set_startpos();
        is >> mode;  // "moves", "pgn4" or nothing
    } else if (tok == "fen") {
        while (is >> tok) {
            if (tok == "moves" || tok == "pgn4") {
                mode = tok;
                break;
            }
            fen += tok;
        }
        std::string err;
        if (!p.set(fen, &err)) {
            std::cout << "info string invalid fen: " << err << std::endl;
            return;
        }
    } else
        return;
    G.reset(p);
    if (mode == "moves")
        while (is >> tok)
            if (!apply_token(G, tok)) {
                std::cout << "info string illegal move " << tok << std::endl;
                break;
            }
    if (mode == "pgn4") {
        Pgn4Replay  replay(G);
        std::string movers;
        int         index = 0;
        while (is >> tok) {
            // move numbers and ".." separators are not tokens
            if (tok == ".." || (tok.size() > 1 && tok.back() == '.' && std::all_of(tok.begin(), tok.end() - 1, ::isdigit)))
                continue;
            Pgn4Step s = replay.step(tok);
            if (s.kind == STEP_ERROR) {
                std::cout << "info string pgn4 error " << s.error << " at " << index << " token " << tok << std::endl;
                return;
            }
            movers += s.mover == NO_COLOR ? '-' : ColorChar[s.mover];
            index++;
        }
        std::cout << "info string pgn4 ok movers " << (movers.empty() ? "-" : movers) << std::endl;
    }
}

static void cmd_go(std::istringstream& is) {
    SearchLimits lim;
    std::string  tok;
    while (is >> tok) {
        auto next_int = [&]() -> long long {
            long long v = 0;
            is >> v;
            return v;
        };
        if (tok == "perft") {
            int d = int(next_int());
            perft_divide(G.pos, d);
            return;
        } else if (tok == "depth")
            lim.depth = int(next_int());
        else if (tok == "nodes")
            lim.nodes = uint64_t(next_int());
        else if (tok == "movetime")
            lim.movetime = int(next_int());
        else if (tok == "infinite")
            lim.infinite = true;
        else if (tok.size() == 5 && tok.substr(1) == "time") {
            int c = std::string("rbyg").find(tok[0]);
            if (c >= 0 && c < 4) lim.time[c] = int(next_int());
        } else if (tok.size() == 4 && tok.substr(1) == "inc") {
            int c = std::string("rbyg").find(tok[0]);
            if (c >= 0 && c < 4) lim.inc[c] = int(next_int());
        }
    }
    Game snapshot = G;
    if (snapshot.pos.game_over()) {
        // chess.com may not agree that the game is over (rare rule differences): play on
        std::cout << "info string engine thinks the game is over (" << end_reason_name(snapshot.pos.end_reason())
                  << "); searching on" << std::endl;
        snapshot.pos.reopen();
    }
    if (snapshot.pos.resign_wins(snapshot.pos.side_to_move())) {
        std::cout << "info string resigning now wins (claim)\nbestmove resign" << std::endl;
        return;
    }
    if (!snapshot.pos.is_alive(snapshot.pos.side_to_move())) {
        // A zombie: any legal king move (match runners pick zombie moves themselves).
        Move list[MAX_MOVES];
        int  n = snapshot.pos.generate_legal(list);
        std::cout << "bestmove " << (n ? move_to_string(list[0]) : "(none)") << std::endl;
        return;
    }
    StopFlag     = false;
    Searching    = true;
    SearchThread.start([snapshot, lim]() {
        SearchResult r = think(snapshot, lim, StopFlag, true);
        sync_print("bestmove " + (r.resign ? std::string("resign") : move_to_string(r.best)));
    });
}

static void cmd_setoption(std::istringstream& is) {
    std::string tok, name, value;
    is >> tok;  // "name"
    while (is >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
    while (is >> tok) value += (value.empty() ? "" : " ") + tok;
    if (name == "Hash") {
        HashMB = std::max(1, std::atoi(value.c_str()));
        TT.resize(HashMB);
    } else if (name == "SearchMode") {
        if (!parse_mode(value, SOpts.mode)) std::cout << "info string unknown SearchMode " << value << std::endl;
    } else if (name == "MoveOverhead")
        SOpts.moveOverhead = std::atoi(value.c_str());
    else
        std::cout << "No such option: " << name << std::endl;
}

void uci_loop() {
    TT.resize(HashMB);
    G.reset([] {
        Position p;
        p.set_startpos();
        return p;
    }());
    // std::cin is tied to std::cout, so every getline would flush std::cout from this thread while the
    // search thread may be writing to it. Every line we print is flushed anyway.
    std::cin.tie(nullptr);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string        cmd;
        is >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "quit") {
            StopFlag = true;
            wait_search();
            break;
        }
        if (cmd == "stop") {
            StopFlag = true;
            wait_search();
            continue;
        }
        if (cmd == "isready") {
            sync_print("readyok");  // may come while the search thread prints
            continue;
        }
        // every other command waits for a running search to finish
        if (Searching && cmd != "go") wait_search();
        if (cmd == "uci") {
            std::cout << "id name FFA-Engine 0.1\nid author Anurag-Baundwal\n"
                      << "option name Hash type spin default 16 min 1 max 4096\n"
                      << "option name SearchMode type combo default brs+ var brs+ var paranoid\n"
                      << "option name MoveOverhead type spin default 30 min 0 max 5000\n"
                      << "uciok" << std::endl;
        } else if (cmd == "ucinewgame") {
            search_clear();
        } else if (cmd == "setoption")
            cmd_setoption(is);
        else if (cmd == "position")
            cmd_position(is);
        else if (cmd == "go") {
            wait_search();
            cmd_go(is);
        } else if (cmd == "d") {
            std::cout << G.pos.pretty() << "Fen: " << G.pos.fen() << "\nKey: " << std::hex << std::setw(16)
                      << std::setfill('0') << G.pos.key() << std::dec << std::setfill(' ') << std::endl;
        } else if (cmd == "legalmoves") {
            Move list[MAX_MOVES];
            int  n = G.pos.generate_legal(list);
            std::cout << "legalmoves";
            for (int i = 0; i < n; ++i) std::cout << " " << move_to_string(list[i]);
            std::cout << std::endl;
        } else if (cmd == "gamestate")
            std::cout << game_state_line(G.pos) << std::endl;
        else if (cmd == "resignwins") {
            std::string c;
            is >> c;
            int  col = c.empty() ? int(G.pos.side_to_move()) : int(std::string("rbyg").find(c[0]));
            bool yes = col >= 0 && col < 4 && G.pos.resign_wins(Color(col));
            std::cout << "resignwins " << (yes ? "yes" : "no") << std::endl;
        }
        else if (cmd == "eval")
            std::cout << eval_trace(G.pos, G.pos.side_to_move()) << std::flush;
        else
            std::cout << "info string unknown command " << cmd << std::endl;
    }
    wait_search();
}

}  // namespace FFA
