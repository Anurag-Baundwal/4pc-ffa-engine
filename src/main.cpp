/*
  4PC FFA engine.

  ffa_engine                      UCI-like protocol on stdin/stdout (see uci.cpp)
  ffa_engine perft N [FEN4]       perft with per-move counts
  ffa_engine audit FILES...       replay chess.com games and check the rules (see audit.cpp)
  ffa_engine bench [DEPTH]        fixed-depth search (default 4) of 13 positions; prints total nodes
*/

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include "bitboard.h"
#include "position.h"
#include "search.h"
#include "uci.h"

namespace FFA {
int run_audit(int argc, char** argv);
}

using namespace FFA;

static int run_bench(int depth) {
    // Start position, a middlegame, then positions from arena games: four, three and two players alive,
    // dead players and a zombie king.
    const char* fens[] = {
        "",  // start position
        "R-0,0,0,0-1,1,1,0-1,1,1,0-0,0,0,0-0-x,x,x,yR,1,yB,yK,3,yR,x,x,x/x,x,x,yP,yP,yP,1,yP,yP,1,yP,x,x,x/x,x,x,3,yP,2,yP,1,x,x,x/"
        "bR,bP,2,yQ,6,gP,2/1,bP,2,rQ,7,yN,gR/bB,bP,6,bQ,3,gP,1/1,bP,6,gQ,2,gP,gK,1/bK,1,bP,8,gP,2/1,bP,bN,9,gP,gB/1,bP,10,gP,1/"
        "bR,2,bP,2,rP,5,gP,gR/x,x,x,rN,1,rP,5,x,x,x/x,x,x,rP,rP,2,rP,rP,rP,rP,x,x,x/x,x,x,rR,3,rK,rB,1,rR,x,x,x",
        "R-0,0,0,1-0,0,0,0-0,0,0,0-25,5,5,0-3-x,x,x,yR,yN,2,yK,yQ,1,yR,x,x,x/x,x,x,yP,3,yN,2,yP,x,x,x/x,x,x,2,yP,2,yP,2"
        ",x,x,x/bR,1,bP,1,yP,1,yP,yP,1,yP,2,dgP,dgR/1,bP,10,dgP,rQ/bB,bP,10,dgP,1/bQ,1,bP,1,bN,6,yB,2/4,bP,9/bK,bP,bN,8"
        ",dgP,1,dgB/1,bP,10,dgP,dgN/bR,1,bP,2,rP,rP,5,dgP,dgR/x,x,x,4,rP,3,x,x,x/x,x,x,rP,rP,1,rB,1,rP,rP,rP,x,x,x/x,x,"
        "x,rR,rN,1,rK,1,rB,rN,rR,x,x,x",
        "R-0,0,1,0-0,0,0,0-1,0,0,1-1,15,3,21-1-x,x,x,1,dyN,dyB,2,dyB,dyN,dyR,x,x,x/x,x,x,1,dyP,1,dyK,1,dyQ,dyP,1,x,x,x/"
        "x,x,x,2,dyP,1,dyP,2,dyP,x,x,x/bR,bP,1,bQ,2,dyP,1,dyP,1,gP,1,gR,1/1,bP,9,gP,gB,1/bB,1,bP,9,gP,1/bK,1,bP,8,gP,1,"
        "gK/1,bP,6,rD,gP,gN,1,gN,1/bB,1,bP,8,gP,1,gB/bN,bP,12/bR,3,bP,2,rQ,2,rR,gP,1,gR/x,x,x,3,rB,4,x,x,x/x,x,x,rP,rP,"
        "rP,rP,1,rP,rP,1,x,x,x/x,x,x,rR,rN,rB,1,rK,1,rN,1,x,x,x",
        "G-0,0,1,0-0,0,0,0-0,0,0,0-33,23,0,27-1-x,x,x,dyR,dyN,1,dyK,dyQ,dyB,dyN,dyR,x,x,x/x,x,x,gB,dyP,2,dyP,dyP,dyP,dy"
        "P,x,x,x/x,x,x,8,x,x,x/bR,2,bP,2,dyP,5,gP,gR/bK,bB,10,gP,gN/3,gQ,8,gP,1/3,bP,10/4,bP,5,gP,1,gK,1/3,rR,7,gP,2/4,"
        "bP,5,gP,3/bR,9,rP,3/x,x,x,bP,4,rP,2,x,x,x/x,x,x,4,rN,3,x,x,x/x,x,x,4,rQ,rK,2,x,x,x",
        "R-0,0,0,0-0,0,0,0-0,1,0,0-14,6,13,10-5-x,x,x,yR,1,yB,yK,yQ,3,x,x,x/x,x,x,3,yN,yB,yP,2,x,x,x/x,x,x,1,yP,6,x,x,x"
        "/bR,1,bP,2,yP,yP,2,gP,3,gR/7,yP,6/2,bQ,yP,1,yR,8/5,bP,3,gP,4/bK,bN,1,bP,10/bN,bP,3,bB,rP,3,gP,3/1,bR,bP,6,gP,1"
        ",gK,1,gN/1,bP,rN,3,gB,rP,1,rP,gP,2,gR/x,x,x,rP,rP,3,rQ,1,rP,x,x,x/x,x,x,rR,4,rP,2,x,x,x/x,x,x,1,rN,1,rK,rR,3,x"
        ",x,x",
        "Y-0,0,0,0-0,0,0,0-0,0,0,0-7,20,19,14-3-x,x,x,3,yK,1,yR,2,x,x,x/x,x,x,yR,1,yN,yN,4,x,x,x/x,x,x,1,yP,yP,yP,1,yP,"
        "1,yP,x,x,x/1,bN,5,yP,yQ,1,gP,3/1,bQ,9,gP,2/4,bP,4,yB,2,gK,1/bK,1,bB,1,bP,7,gN,1/10,gP,3/2,bP,1,rP,bN,5,gP,2/1,"
        "bP,4,rN,7/bR,1,bP,rP,1,rP,1,rP,2,gP,2,gR/x,x,x,5,rP,gN,rN,x,x,x/x,x,x,8,x,x,x/x,x,x,1,rR,rQ,1,rK,1,rR,1,x,x,x",
        "R-0,0,0,0-0,0,0,0-0,0,0,0-27,3,23,12-1-x,x,x,1,yN,3,yB,2,x,x,x/x,x,x,3,yK,yN,3,x,x,x/x,x,x,1,yR,yB,1,yP,yP,2,x"
        ",x,x/13,yQ/2,bP,2,yP,4,gP,gQ,1,gN/7,rN,6/bQ,2,bP,7,gP,gK,1/11,gP,gB,1/2,bN,3,yD,7/11,gP,1,gR/bK,1,rP,8,gP,2/x,"
        "x,x,2,rP,rP,rQ,rP,2,x,x,x/x,x,x,6,rB,rP,x,x,x/x,x,x,rR,4,rK,1,rR,x,x,x",
        "R-0,0,0,1-0,0,0,0-0,0,0,0-12,34,46,11-0-x,x,x,1,yN,yK,3,yN,1,x,x,x/x,x,x,4,yP,3,x,x,x/x,x,x,1,yP,3,yP,2,x,x,x/"
        "bR,2,yR,1,yP,4,yR,dgN,1,dgR/bN,bP,2,bP,6,yQ,2/9,dgQ,2,dgP,1/bQ,1,bK,bP,10/6,yD,7/4,bD,7,dgK,1/6,rP,1,rP,rP,3,d"
        "gR/14/x,x,x,bR,3,rP,2,yB,x,x,x/x,x,x,4,rK,3,x,x,x/x,x,x,8,x,x,x",
        "Y-1,0,0,1-0,0,0,0-0,0,0,0-25,45,35,41-1-x,x,x,yR,1,yK,yN,1,bD,2,x,x,x/x,x,x,yP,1,yP,yB,yR,3,x,x,x/x,x,x,1,yP,5"
        ",yP,x,x,x/9,yP,dgP,3/11,dgP,2/6,yD,yP,6/2,bP,4,bR,6/1,bK,4,dgD,3,yQ,dgP,2/14/14/5,bP,7,dgR/x,x,x,7,drK,x,x,x/x"
        ",x,x,3,drP,4,x,x,x/x,x,x,2,drB,5,x,x,x",
        "R-0,0,0,0-0,0,0,0-0,0,0,0-10,1,6,5-1-x,x,x,1,yR,yB,yK,1,yB,yN,yR,x,x,x/x,x,x,1,yP,yQ,yN,yP,1,yP,1,x,x,x/x,x,x,"
        "yP,1,yP,4,yP,x,x,x/bR,bP,4,yP,3,gP,2,gR/bN,1,bP,5,yP,3,gP,gK/2,bP,7,gP,3/1,bP,5,gN,1,gB,1,gP,2/2,bP,2,bQ,2,gP,"
        "3,gN,gQ/1,bK,1,bP,1,rP,6,gP,gR/bN,1,bP,6,rP,2,gP,1/1,bR,1,bP,2,rP,1,rN,1,rP,gP,2/x,x,x,rP,4,rQ,2,x,x,x/x,x,x,1"
        ",rP,1,rN,2,rB,1,x,x,x/x,x,x,rR,1,rK,4,rR,x,x,x",
        "Y-1,0,0,0-0,0,0,0-0,0,0,0-15,40,22,21-1-x,x,x,yK,yR,6,x,x,x/x,x,x,8,x,x,x/x,x,x,1,yR,6,x,x,x/bR,bP,8,yP,3/bN,2"
        ",bP,10/bK,9,gP,3/1,bB,3,bP,3,gP,4/bN,2,bP,1,drB,bP,gP,6/11,gP,drQ,1/bR,8,gP,2,gK,1/3,bP,1,drP,drP,drP,1,gQ,gP,"
        "bQ,2/x,x,x,drP,drP,6,x,x,x/x,x,x,3,drN,1,drP,2,x,x,x/x,x,x,drR,3,drR,drK,2,x,x,x",
        "B-0,0,0,0-0,0,0,0-0,0,0,0-34,34,6,28-1-{'resigned':(null,null,null,true)}-x,x,x,8,x,x,x/x,x,x,7,yK,x,x,x/x,x,x"
        ",8,x,x,x/bR,3,bP,9/bN,8,dgP,4/12,gK,1/4,dgR,6,dgB,2/1,bK,9,bD,2/bR,1,bP,8,dgP,2/4,rP,1,rP,3,dgP,2,dgN/2,bQ,3,r"
        "P,1,rP,1,dgR,3/x,x,x,2,rN,5,x,x,x/x,x,x,2,rP,2,rK,1,rP,x,x,x/x,x,x,3,rB,3,rR,x,x,x",
    };
    uint64_t total = 0;
    auto     t0 = std::chrono::steady_clock::now();
    for (const char* f : fens) {
        Position p;
        if (*f) p.set(f);
        else p.set_startpos();
        Game g;
        g.reset(p);
        search_clear();
        SearchLimits lim;
        lim.depth = depth;
        std::atomic<bool> stop{false};
        SearchResult r = think(g, lim, stop, false);
        total += r.nodes;
        std::cout << "bestmove " << move_to_string(r.best) << " score " << r.score << " nodes " << r.nodes << "\n";
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "Nodes: " << total << "\nNPS: " << total * 1000 / std::max<long long>(1, ms) << "\nBench: " << total << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    Bitboards::init();
    Position::init();
    std::ios::sync_with_stdio(false);

    if (argc >= 2 && !std::strcmp(argv[1], "perft")) {
        int      depth = argc >= 3 ? std::atoi(argv[2]) : 4;
        Position p;
        std::string err;
        if (argc >= 4 && !p.set(argv[3], &err)) {
            std::cerr << "bad fen: " << err << "\n";
            return 1;
        }
        if (argc < 4) p.set_startpos();
        perft_divide(p, depth);
        return 0;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "audit")) return run_audit(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "bench")) return run_bench(argc >= 3 ? std::atoi(argv[2]) : 4);

    uci_loop();
    return 0;
}
