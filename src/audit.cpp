/*
  4PC FFA engine - rules audit against chess.com PGN4 games.

  ffa_engine audit <pgn files...> [--max N] [--examples K] [--game ID] [--openings OUT --opening-ply P]

  Replays every standard FFA game (StartFen4 "4PC", RuleVariants "DeadKingWalking EnPassant PromoteTo=D",
  ignoring Anonymous/SemiAnonymous) and checks:
    - every move token is one of the engine's legal moves,
    - check / mate / stalemate annotations ('+' per king, '#', bare 'S') agree with the engine's events,
    - the final points agree with the Result tag (using the engine's own game end if it ended the game,
      otherwise the engine's points plus the end bonus named in the Termination tag).
  With --openings, also writes the FEN4 after P plies of each game that replays cleanly (one per line).
*/

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pgn4.h"
#include "position.h"
#include "uci.h"

namespace FFA {

namespace {

struct PgnGame {
    std::map<std::string, std::string> tags;
    std::string                        movetext;
};

// Reads games from a multi-game PGN4 file. Calls f for each game; stops if f returns false.
template<typename F>
void read_pgn_file(const std::string& path, F&& f) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return;
    }
    PgnGame     g;
    bool        inGame = false, inMoves = false;
    std::string line;
    auto flush = [&]() -> bool {
        bool ok = true;
        if (inGame) ok = f(g);
        g       = PgnGame();
        inGame  = inMoves = false;
        return ok;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && line[0] == '[') {
            if (inMoves || (line.rfind("[GameNr", 0) == 0 && inGame))
                if (!flush()) return;
            inGame = true;
            size_t sp = line.find(' ');
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            if (sp != std::string::npos && q1 != std::string::npos && q2 > q1)
                g.tags[line.substr(1, sp - 1)] = line.substr(q1 + 1, q2 - q1 - 1);
        } else if (!line.empty() && inGame) {
            inMoves = true;
            g.movetext += line;
            g.movetext += ' ';
        }
    }
    flush();
}

bool is_standard(const PgnGame& g) {
    auto it = g.tags.find("StartFen4");
    if (it == g.tags.end() || it->second != "4PC") return false;
    auto rv = g.tags.find("RuleVariants");
    if (rv == g.tags.end()) return false;
    std::set<std::string> flags;
    std::istringstream    is(rv->second);
    std::string           t;
    while (is >> t)
        if (t != "Anonymous" && t != "SemiAnonymous") flags.insert(t);
    static const std::set<std::string> std_flags = {"DeadKingWalking", "EnPassant", "PromoteTo=D"};
    if (flags != std_flags) return false;
    auto res = g.tags.find("Result");
    return res != g.tags.end() && res->second != "Aborted";
}

// "name: 12 - other-name: 3 - ..." in seat order; names may contain '-'.
std::vector<std::pair<std::string, int>> parse_result(const std::string& r) {
    std::vector<std::pair<std::string, int>> out;
    size_t pos = 0;
    while (pos < r.size()) {
        size_t colon = r.find(": ", pos);
        if (colon == std::string::npos) break;
        size_t end = r.find(" - ", colon);
        std::string name = r.substr(pos, colon - pos);
        std::string num  = r.substr(colon + 2, end == std::string::npos ? std::string::npos : end - colon - 2);
        try { out.push_back({name, std::stoi(num)}); } catch (...) { return {}; }
        if (end == std::string::npos) break;
        pos = end + 3;
    }
    return out;
}

// End bonuses named in a Termination tag: "(Red +20)", "(insuff. material; Red +10, Yellow +10)".
bool termination_bonus(const std::string& term, int bonus[COLOR_NB]) {
    for (int c = 0; c < COLOR_NB; ++c) bonus[c] = 0;
    bool any = false;
    for (int c = 0; c < COLOR_NB; ++c) {
        std::string key = std::string(ColorName[c]) + " +";
        size_t      p   = term.find(key);
        if (p != std::string::npos) {
            bonus[c] = std::atoi(term.c_str() + p + key.size());
            any      = true;
        }
    }
    return any;
}

struct Stats {
    long games = 0, replayed = 0, illegal = 0, parseErr = 0, earlyEnd = 0;
    long plusMismatch = 0, plusMismatchExMated = 0, hashMismatch = 0, staleMismatch = 0, moves = 0;
    long ptsOkEngineEnd = 0, ptsBadEngineEnd = 0, ptsOkTag = 0, ptsBadTag = 0, ptsUnknown = 0;
    std::map<std::string, long> endCats;
    std::map<std::string, int>  printed;
};

std::string colors_mask(uint8_t m) {
    std::string s;
    for (int c = 0; c < 4; ++c)
        if (m & (1 << c)) s += ColorChar[c];
    return s.empty() ? "-" : s;
}

}  // namespace

int run_audit(int argc, char** argv) {
    std::vector<std::string> files;
    long        maxGames = -1;
    int         examples = 5;
    std::string onlyGame, openingsOut;
    int         openingPly = 12;
    bool        resignAfterMove = false, verify = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--max" && i + 1 < argc) maxGames = std::atol(argv[++i]);
        else if (a == "--examples" && i + 1 < argc) examples = std::atoi(argv[++i]);
        else if (a == "--game" && i + 1 < argc) onlyGame = argv[++i];
        else if (a == "--openings" && i + 1 < argc) openingsOut = argv[++i];
        else if (a == "--opening-ply" && i + 1 < argc) openingPly = std::atoi(argv[++i]);
        else if (a == "--resign-after-move") resignAfterMove = true;
        else if (a == "--verify") verify = true;
        else if (a == "--rule" && i + 1 < argc) {
            std::string r = argv[++i];
            size_t      eq = r.find('=');
            if (eq == std::string::npos || !set_rule(r.substr(0, eq), std::atoi(r.c_str() + eq + 1))) {
                std::cerr << "unknown rule " << r << "\n";
                return 1;
            }
        }
        else files.push_back(a);
    }
    std::ofstream openings;
    if (!openingsOut.empty()) openings.open(openingsOut);
    std::cout << "rules: " << rules_string() << "\n";

    Stats st;
    auto  report = [&](const std::string& cat, const std::string& text) {
        if (st.printed[cat]++ < examples || !onlyGame.empty()) std::cout << "[" << cat << "] " << text << "\n";
    };

    bool stop = false;
    for (auto& file : files) {
        if (stop) break;
        read_pgn_file(file, [&](const PgnGame& g) {
            if (maxGames >= 0 && st.games >= maxGames) {
                stop = true;
                return false;
            }
            std::string id = g.tags.count("GameNr") ? g.tags.at("GameNr") : "?";
            if (!onlyGame.empty() && id != onlyGame) return true;
            if (!is_standard(g)) return true;
            st.games++;
            bool verbose = !onlyGame.empty();

            Game game;
            {
                Position p;
                p.set_startpos();
                game.reset(p);
            }
            auto tokens = tokenize_pgn4(g.movetext);
            int  ply = 0;
            bool bad = false;
            std::string openingFen, eventLog;
            const std::string termTag = g.tags.count("Termination") ? g.tags.at("Termination") : "";
            const bool        insuffEnd = termTag.find("insuff. material") != std::string::npos;
            auto log_events = [&](const Position& before, const MoveEvents& e, Color mover) {
                auto st_name = [&](Color c) { return before.status(c) == ALIVE ? "A" : before.status(c) == ZOMBIE ? "Z" : "D"; };
                for (int c = 0; c < 4; ++c) {
                    if (e.stalemateMask & (1 << c)) eventLog += " S:" + std::string(1, ColorChar[c]) + st_name(Color(c));
                    if (e.matedMask & (1 << c)) eventLog += " M:" + std::string(1, ColorChar[c]) + st_name(Color(c)) + "by" + ColorChar[mover] + st_name(mover);
                    if (e.kingCapMask & (1 << c)) eventLog += " K:" + std::string(1, ColorChar[c]) + st_name(Color(c)) + "by" + ColorChar[mover];
                    if (e.turnMateMask & (1 << c)) eventLog += " T:" + std::string(1, ColorChar[c]) + st_name(Color(c));
                }
            };
            Pgn4Replay replay(game);
            for (size_t ti = 0; ti < tokens.size() && !bad; ++ti) {
                const std::string& t = tokens[ti];
                const Pgn4Token    k = parse_pgn4_token(t);
                const Position     before = game.pos;
                Pgn4Step           s = replay.step(t, ti + 1 == tokens.size(), insuffEnd, resignAfterMove);
                for (auto& note : s.notes) {
                    bool stale = note.rfind("stalemate", 0) == 0;
                    (stale ? st.staleMismatch : st.hashMismatch)++;
                    report(note, id + " ply " + std::to_string(ply) + " token '" + t + "'; fen " + before.fen());
                }
                if (s.kind == STEP_ERROR) {
                    if (s.error == "parse") {
                        st.parseErr++;
                        report("parse", id + " token '" + t + "'");
                    } else if (s.error == "game-over") {
                        st.earlyEnd++;
                        report("engine-ended-early", id + " ply " + std::to_string(ply) + " reason " + end_reason_name(game.pos.end_reason()) +
                                                        ", PGN continues with '" + t + "' (" + std::to_string(tokens.size() - ti) + " tokens left); term '" + termTag + "'");
                    } else {
                        st.illegal++;
                        Move list[MAX_MOVES];
                        int  n = game.pos.generate_legal(list);
                        std::string lm;
                        for (int i = 0; i < n && i < 60; ++i) lm += move_to_string(list[i]) + " ";
                        report("illegal", id + " ply " + std::to_string(ply) + " " + ColorName[game.pos.side_to_move()] + " '" + t + "' not legal; fen " + game.pos.fen() + "\n    legal: " + lm);
                    }
                    bad = true;
                    break;
                }
                if (s.kind == STEP_END) {
                    eventLog += " I";
                    continue;
                }
                if (s.kind == STEP_MARKER) continue;
                if (k.resignBare || k.turnMateBare || k.resignAfter) eventLog += " R:" + std::string(1, ColorChar[s.mover]);
                if (s.kind == STEP_RESIGN) {
                    log_events(before, s.events, s.mover);
                    if (k.resignBare || k.turnMateBare) ply++;
                    if (verbose) std::cout << ply << " " << ColorChar[s.mover] << " " << t << "  -> " << game_state_line(game.pos) << "\n";
                    continue;
                }
                const MoveEvents& ev = s.events;
                log_events(before, ev, s.mover);
                st.moves++;
                ply++;
                if (verify) {
                    const Position& p = game.pos;
                    if (!p.hash_ok()) report("verify-hash", id + " ply " + std::to_string(ply) + " incremental hash differs; fen " + p.fen());
                    for (int c = 0; c < 4 && !p.game_over(); ++c) {
                        Move buf[MAX_MOVES];
                        bool full = p.generate_legal_for(Color(c), buf, false) > 0;
                        if (full != p.has_legal_move(Color(c)))
                            report("verify-hasmove", id + " ply " + std::to_string(ply) + " color " + ColorChar[c] + "; fen " + p.fen());
                    }
                    Position q;
                    std::string err;
                    if (!q.set(p.fen(), &err))
                        report("verify-fen", id + " ply " + std::to_string(ply) + " fen does not parse: " + err + "; " + p.fen());
                    else if (q.fen() != p.fen() || (!p.game_over() && q.key() != p.key()))
                        report("verify-fen", id + " ply " + std::to_string(ply) + " round trip differs:\n    " + p.fen() + "\n    " + q.fen());
                }
                int checked = popcount(Bitboard(ev.checkedMask)), mated = popcount(Bitboard(uint64_t(ev.matedMask | ev.kingCapMask)));
                int checkedExMated = popcount(Bitboard(uint64_t(ev.checkedMask & ~(ev.matedMask | ev.kingCapMask))));
                if (k.plus != checked) {
                    st.plusMismatch++;
                    report("plus", id + " ply " + std::to_string(ply) + " '" + t + "' engine checked " + colors_mask(ev.checkedMask) + " mated " + colors_mask(ev.matedMask) + " kingcap " + colors_mask(ev.kingCapMask));
                }
                if (k.plus != checkedExMated) st.plusMismatchExMated++;
                if (k.hash != mated) {
                    st.hashMismatch++;
                    report("hash", id + " ply " + std::to_string(ply) + " '" + t + "' engine mated " + colors_mask(ev.matedMask) + " kingcap " + colors_mask(ev.kingCapMask) + " checked " + colors_mask(ev.checkedMask));
                }
                if (verbose)
                    std::cout << ply << " " << ColorChar[s.mover] << " " << t << " (" << move_to_string(s.move) << ")  checked " << colors_mask(ev.checkedMask)
                              << " mated " << colors_mask(ev.matedMask) << " stalemated " << colors_mask(ev.stalemateMask) << " bonus " << ev.checkBonus
                              << " cap " << ev.captureValue << "  -> " << game_state_line(game.pos) << "\n";
                if (ply == openingPly && game.pos.alive_count() == 4 && !game.pos.game_over()) openingFen = game.pos.fen();
            }
            if (bad) return true;
            if (!replay.pendingStalemates.empty() && !game.pos.game_over()) {
                st.staleMismatch++;
                report("stalemate-extra", id + " at end: engine stalemated a player, PGN has no 'S'");
            }
            st.replayed++;
            if (openings.is_open() && !openingFen.empty()) openings << openingFen << "\n";

            // Points
            std::string term = g.tags.count("Termination") ? g.tags.at("Termination") : "";
            auto        res  = parse_result(g.tags.count("Result") ? g.tags.at("Result") : "");
            int         expected[COLOR_NB];
            bool        haveExpected = res.size() == 4;
            if (haveExpected) {
                const char* tagName[4] = {"Red", "Blue", "Yellow", "Green"};
                std::map<std::string, int> byName;
                std::set<std::string>      names;
                bool unique = true;
                for (int c = 0; c < 4; ++c) {
                    std::string nm = g.tags.count(tagName[c]) ? g.tags.at(tagName[c]) : "";
                    unique &= names.insert(nm).second;
                }
                for (auto& [nm, p] : res) byName[nm] = p;
                for (int c = 0; c < 4; ++c) {
                    std::string nm = g.tags.count(tagName[c]) ? g.tags.at(tagName[c]) : "";
                    expected[c] = (unique && byName.count(nm)) ? byName[nm] : res[c].second;
                }
            }
            int  engine[COLOR_NB], bonus[COLOR_NB];
            bool tagBonus = termination_bonus(term, bonus);
            std::string cat;
            if (game.pos.game_over()) {
                for (int c = 0; c < 4; ++c) engine[c] = game.pos.points(Color(c));
                cat = std::string("engine:") + end_reason_name(game.pos.end_reason());
            } else {
                for (int c = 0; c < 4; ++c) engine[c] = game.pos.points(Color(c)) + bonus[c];
                std::string tcat = term;
                if (tagBonus) tcat = term.find("insuff") != std::string::npos ? "(insuff+bonus)" : "(+bonus)";
                cat = "tag:" + tcat;
            }
            bool match = haveExpected;
            for (int c = 0; c < 4 && haveExpected; ++c) match &= engine[c] == expected[c];
            st.endCats[cat + (match ? " ok" : " BAD")]++;
            if (!haveExpected) st.ptsUnknown++;
            else if (game.pos.game_over()) (match ? st.ptsOkEngineEnd : st.ptsBadEngineEnd)++;
            else (match ? st.ptsOkTag : st.ptsBadTag)++;
            if (haveExpected && !match) {
                std::ostringstream os;
                os << id << " " << cat << " term '" << term << "' engine";
                for (int c = 0; c < 4; ++c) os << " " << engine[c];
                os << " expected";
                for (int c = 0; c < 4; ++c) os << " " << expected[c];
                os << " delta";
                for (int c = 0; c < 4; ++c) os << " " << expected[c] - engine[c];
                os << " | " << game_state_line(game.pos) << " endbonus";
                for (int c = 0; c < 4; ++c) os << " " << game.pos.end_bonus(Color(c));
                os << " | events" << eventLog;
                report("points " + cat, os.str());
            }
            if (verbose) std::cout << game.pos.pretty() << "fen " << game.pos.fen() << "\n";
            if (verbose) std::cout << "final: " << game_state_line(game.pos) << " | term '" << term << "' result '" << g.tags.at("Result") << "'\n";
            return true;
        });
    }

    std::cout << "\n=== audit summary ===\n"
              << "standard games: " << st.games << ", replayed to the end: " << st.replayed << ", moves: " << st.moves << "\n"
              << "illegal moves: " << st.illegal << ", parse errors: " << st.parseErr << ", engine ended early: " << st.earlyEnd << "\n"
              << "'+' mismatches: " << st.plusMismatch << " (counting mated kings as not checked: " << st.plusMismatchExMated << ")\n"
              << "'#' mismatches: " << st.hashMismatch << ", stalemate mismatches: " << st.staleMismatch << "\n"
              << "points: engine-ended ok " << st.ptsOkEngineEnd << " bad " << st.ptsBadEngineEnd << "; tag-ended ok " << st.ptsOkTag
              << " bad " << st.ptsBadTag << "; unknown " << st.ptsUnknown << "\n"
              << "end categories:\n";
    for (auto& [k, v] : st.endCats) std::cout << "  " << v << "  " << k << "\n";
    return 0;
}

}  // namespace FFA
