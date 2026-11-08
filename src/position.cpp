/*
  4PC FFA engine - board state, rules and move generation.
  Castling, en passant and FEN4 conventions follow stockfish_4pc (which matches chess.com).
*/

#include "position.h"

#include <cstring>
#include <sstream>

namespace FFA {

RuleConfig Rules;

namespace {
struct RuleField {
    const char* name;
    int*        i;
    bool*       b;
};
std::vector<RuleField> rule_fields() {
    RuleConfig& r = Rules;
    return {{"promotedValue", &r.promotedValue, nullptr},     {"grayValue", &r.grayValue, nullptr},
            {"grayKingValue", &r.grayKingValue, nullptr},     {"zombieKingValue", &r.zombieKingValue, nullptr},
            {"zombieStalemateSplit", &r.zombieStalemateSplit, nullptr},
            {"mateBonus", &r.mateBonus, nullptr},             {"stalemateBonus", &r.stalemateBonus, nullptr},
            {"checkBonus2", &r.checkBonus2, nullptr},         {"checkBonus3", &r.checkBonus3, nullptr},
            {"zombiesCountForChecks", nullptr, &r.zombiesCountForChecks},
            {"zombieKingAttacks", nullptr, &r.zombieKingAttacks}, {"zombieScores", nullptr, &r.zombieScores},
            {"endBonusPerKing", &r.endBonusPerKing, nullptr}, {"endBonusCountsDeadKings", nullptr, &r.endBonusCountsDeadKings},
            {"drawBonus", &r.drawBonus, nullptr},             {"rule50Moves", &r.rule50Moves, nullptr},
            {"promotionRelRank", &r.promotionRelRank, nullptr}};
}
}  // namespace

bool set_rule(const std::string& name, int value) {
    for (auto& f : rule_fields())
        if (name == f.name) {
            if (f.i) *f.i = value;
            else *f.b = value != 0;
            return true;
        }
    return false;
}

std::string rules_string() {
    std::string s;
    for (auto& f : rule_fields()) s += std::string(f.name) + "=" + std::to_string(f.i ? *f.i : int(*f.b)) + " ";
    return s;
}

namespace {

struct PRNG {
    uint64_t s;
    explicit PRNG(uint64_t seed) : s(seed) {}
    uint64_t rand64() {
        s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
};

namespace Zobrist {
Key psq[PIECE_NB][SQUARE_NB];
Key promoted[SQUARE_NB];
Key side[COLOR_NB];
Key castling[CASTLING_RIGHT_NB];
Key enpassant[SQUARE_NB];
Key status[COLOR_NB][3];
Key points[COLOR_NB][1024];
}  // namespace Zobrist

Bitboard RelRankBB[COLOR_NB][14];  // squares at each relative rank of a color

const std::string StartFen =
    "R-0,0,0,0-1,1,1,1-1,1,1,1-0,0,0,0-0-"
    "x,x,x,yR,yN,yB,yK,yQ,yB,yN,yR,x,x,x/x,x,x,yP,yP,yP,yP,yP,yP,yP,yP,x,x,x/x,x,x,8,x,x,x/"
    "bR,bP,10,gP,gR/bN,bP,10,gP,gN/bB,bP,10,gP,gB/bQ,bP,10,gP,gK/bK,bP,10,gP,gQ/bB,bP,10,gP,gB/"
    "bN,bP,10,gP,gN/bR,bP,10,gP,gR/x,x,x,8,x,x,x/x,x,x,rP,rP,rP,rP,rP,rP,rP,rP,x,x,x/"
    "x,x,x,rR,rN,rB,rQ,rK,rB,rN,rR,x,x,x";

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string              cur;
    for (char ch : s) {
        if (ch == d) {
            out.push_back(cur);
            cur.clear();
        } else
            cur += ch;
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n'\"");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n'\"");
    return s.substr(a, b - a + 1);
}

// Values of a tuple like ('a3:a4','',...) or (true,false,...) for `key` in a FEN4 dict, or empty.
std::vector<std::string> dict_tuple(const std::string& dict, const std::string& key) {
    size_t k = dict.find("'" + key + "'");
    if (k == std::string::npos) return {};
    size_t a = dict.find('(', k), b = dict.find(')', k);
    if (a == std::string::npos || b == std::string::npos || b < a) return {};
    std::vector<std::string> v;
    for (auto& t : split(dict.substr(a + 1, b - a - 1), ',')) v.push_back(trim(t));
    return v;
}

int color_index(char ch) {
    switch (ch) {
    case 'r': case 'R': return RED;
    case 'b': case 'B': return BLUE;
    case 'y': case 'Y': return YELLOW;
    case 'g': case 'G': return GREEN;
    default: return -1;
    }
}

template<Direction D>
Bitboard shift_dyn_helper(const Bitboard& b) { return shift<D>(b); }

}  // namespace

const char* end_reason_name(EndReason r) {
    switch (r) {
    case END_NONE: return "none";
    case END_LAST_PLAYER: return "last_player";
    case END_INSUFFICIENT: return "insufficient_material";
    case END_RULE50: return "rule50";
    case END_REPETITION: return "repetition";
    case END_ADJUDICATED: return "adjudicated";
    }
    return "?";
}

std::string square_to_string(Square s) {
    if (!is_ok(s)) return "-";
    return std::string(1, char('a' + file_of(s) - 1)) + std::to_string(int(rank_of(s)));
}

Square string_to_square(const std::string& s) {
    if (s.size() < 2 || s.size() > 3 || s[0] < 'a' || s[0] > 'n') return SQ_NONE;
    int r = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isdigit((unsigned char)s[i])) return SQ_NONE;
        r = r * 10 + (s[i] - '0');
    }
    if (r < 1 || r > 14) return SQ_NONE;
    Square sq = make_square(File(s[0] - 'a' + 1), Rank(r));
    return is_ok(sq) ? sq : SQ_NONE;
}

std::string move_to_string(Move m) {
    if (m == Move::none()) return "(none)";
    if (m == Move::null()) return "0000";
    std::string s = square_to_string(m.from_sq()) + square_to_string(m.to_sq());
    if (m.is_promotion()) s += 'd';
    return s;
}

// ------------------------------------------------------------------------------------------------
//  Initialization
// ------------------------------------------------------------------------------------------------

void Position::init() {
    PRNG rng(0x4FFA2026ULL);
    for (auto& a : Zobrist::psq)
        for (auto& k : a) k = rng.rand64();
    for (auto& k : Zobrist::promoted) k = rng.rand64();
    for (auto& k : Zobrist::side) k = rng.rand64();
    for (auto& k : Zobrist::castling) k = rng.rand64();
    Zobrist::castling[0] = 0;
    for (auto& k : Zobrist::enpassant) k = rng.rand64();
    for (auto& a : Zobrist::status)
        for (auto& k : a) k = rng.rand64();
    for (auto& a : Zobrist::points)
        for (auto& k : a) k = rng.rand64();

    for (int c = 0; c < COLOR_NB; ++c)
        for (int r = 0; r < 14; ++r) RelRankBB[c][r] = Bitboard::zero();
    for (int s = 0; s < SQUARE_NB; ++s)
        if (is_ok(Square(s)))
            for (int c = 0; c < COLOR_NB; ++c) RelRankBB[c][relative_rank(Color(c), Square(s))].set_bit(s);
}

// ------------------------------------------------------------------------------------------------
//  Piece placement
// ------------------------------------------------------------------------------------------------

void Position::put_piece(Piece pc, Square s) {
    board[s] = uint8_t(pc);
    byTypeBB[ALL_PIECES] |= s;
    byTypeBB[type_of(pc)] |= s;
    byColorBB[color_of(pc)] |= s;
    if (type_of(pc) == KING) kingSq[color_of(pc)] = s;
}

void Position::remove_piece(Square s) {
    Piece pc = piece_on(s);
    byTypeBB[ALL_PIECES] ^= s;
    byTypeBB[type_of(pc)] ^= s;
    byColorBB[color_of(pc)] ^= s;
    if (promotedBB.test(s)) promotedBB ^= s;
    if (type_of(pc) == KING) kingSq[color_of(pc)] = SQ_NONE;
    board[s] = NO_PIECE;
}

void Position::move_piece(Square from, Square to) {
    Piece    pc = piece_on(from);
    Bitboard ft = square_bb(from) | square_bb(to);
    byTypeBB[ALL_PIECES] ^= ft;
    byTypeBB[type_of(pc)] ^= ft;
    byColorBB[color_of(pc)] ^= ft;
    if (promotedBB.test(from)) promotedBB ^= ft;
    board[from] = NO_PIECE;
    board[to]   = uint8_t(pc);
    if (type_of(pc) == KING) kingSq[color_of(pc)] = to;
}

Key Position::compute_key() const {
    Key k = 0;
    Bitboard b = pieces();
    while (b) {
        Square s = pop_lsb(b);
        k ^= Zobrist::psq[piece_on(s)][s];
        if (promotedBB.test(s)) k ^= Zobrist::promoted[s];
    }
    k ^= Zobrist::side[sideToMove];
    k ^= Zobrist::castling[castling];
    for (int c = 0; c < COLOR_NB; ++c) {
        if (epSquares[c] != SQ_NONE) k ^= Zobrist::enpassant[epSquares[c]];
        k ^= Zobrist::status[c][playerStatus[c]];
    }
    return k;
}

Key Position::pointsKey() const {
    Key k = 0;
    for (int c = 0; c < COLOR_NB; ++c) k ^= Zobrist::points[c][pts[c] & 1023];
    return k;
}

bool Position::hash_ok() const { return compute_key() == posKey; }

void Position::add_points(Color c, int n) {
    if (n) pts[c] += n;
}

int Position::alive_count() const {
    int n = 0;
    for (int c = 0; c < COLOR_NB; ++c) n += playerStatus[c] == ALIVE;
    return n;
}

// ------------------------------------------------------------------------------------------------
//  FEN4
// ------------------------------------------------------------------------------------------------

void Position::set_startpos() { set(StartFen); }

void Position::set_castling_right(Color c, Square rfrom) {
    Square ksq = kingSq[c];
    int    dist = (c == RED || c == YELLOW) ? std::abs(file_of(rfrom) - file_of(ksq))
                                            : std::abs(rank_of(rfrom) - rank_of(ksq));
    int    bit  = dist == 3 ? 2 * c : dist == 4 ? 2 * c + 1 : -1;
    if (bit < 0 || !(castling & (1 << bit))) return;
    castlingRookSquare[bit] = rfrom;
    castlingRightsMask[ksq] |= uint8_t(castling_rights_of(c));
    castlingRightsMask[rfrom] |= uint8_t(1 << bit);
}

bool Position::set(const std::string& fenIn, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };

    std::string fen;
    for (char ch : fenIn)
        if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') fen += ch;

    std::string dict;
    size_t      lb = fen.find('{'), rb = fen.rfind('}');
    if (lb != std::string::npos) {
        if (rb == std::string::npos || rb < lb) return fail("unbalanced {}");
        dict = fen.substr(lb, rb - lb + 1);
        fen  = fen.substr(0, lb) + fen.substr(rb + 1);
    }
    std::vector<std::string> parts;
    for (auto& p : split(fen, '-'))
        if (!p.empty()) parts.push_back(p);
    if (parts.size() != 7) return fail("expected 7 fields");

    std::memset(static_cast<void*>(this), 0, sizeof(Position));
    for (auto& b : byTypeBB) b = Bitboard::zero();
    for (auto& b : byColorBB) b = Bitboard::zero();
    for (auto& b : checkersBB) b = Bitboard::zero();
    promotedBB = Bitboard::zero();
    for (int c = 0; c < COLOR_NB; ++c) kingSq[c] = epSquares[c] = SQ_NONE;
    for (auto& s : castlingRookSquare) s = SQ_NONE;

    int stm = color_index(parts[0].empty() ? '?' : parts[0][0]);
    if (stm < 0 || parts[0].size() != 1) return fail("bad side to move");
    sideToMove = Color(stm);

    auto ints4 = [&](const std::string& s, int out[4]) {
        auto v = split(s, ',');
        if (v.size() != 4) return false;
        for (int i = 0; i < 4; ++i) {
            try { out[i] = std::stoi(v[i]); } catch (...) { return false; }
        }
        return true;
    };
    int elim[4], ck[4], cq[4], p[4];
    if (!ints4(parts[1], elim) || !ints4(parts[2], ck) || !ints4(parts[3], cq) || !ints4(parts[4], p))
        return fail("bad header field");
    try { rule50 = std::stoi(parts[5]); } catch (...) { return fail("bad halfmove clock"); }

    castling = 0;
    for (int c = 0; c < 4; ++c) {
        if (ck[c]) castling |= castling_oo(Color(c));
        if (cq[c]) castling |= castling_ooo(Color(c));
        pts[c] = p[c];
    }

    // Board: 14 rows from rank 14 down to rank 1
    auto rows = split(parts[6], '/');
    if (rows.size() != 14) return fail("expected 14 rows");
    bool grayPiece[4] = {false, false, false, false};
    for (int i = 0; i < 14; ++i) {
        Rank r    = Rank(14 - i);
        int  file = 1;
        for (auto& cell : split(rows[i], ',')) {
            if (cell.empty()) continue;
            if (cell == "x") {
                file++;
                continue;
            }
            if (isdigit((unsigned char)cell[0])) {
                file += std::atoi(cell.c_str());
                continue;
            }
            std::string body = cell;
            bool        gray = false;
            if (body.size() == 3 && body[0] == 'd') {
                gray = true;
                body = body.substr(1);
            }
            int c = color_index(body[0]);
            if (body.size() != 2 || c < 0 || (body[0] != ColorChar[c])) return fail("bad piece " + cell);
            PieceType pt = NO_PIECE_TYPE;
            bool      promo = false;
            switch (body[1]) {
            case 'P': pt = PAWN; break;
            case 'N': pt = KNIGHT; break;
            case 'B': pt = BISHOP; break;
            case 'R': pt = ROOK; break;
            case 'Q': pt = QUEEN; break;
            case 'D': pt = QUEEN; promo = true; break;
            case 'K': pt = KING; break;
            default: return fail("bad piece " + cell);
            }
            if (file > 14) return fail("row too long");
            Square s = make_square(File(file), r);
            if (!is_ok(s)) return fail("piece on a missing square");
            put_piece(make_piece(Color(c), pt), s);
            if (promo) promotedBB |= s;
            if (gray) grayPiece[c] = true;
            file++;
        }
        if (file != 15) return fail("row " + std::to_string(i) + " has wrong length");
    }

    // Player states: eliminated (field, gray pieces or resigned/flagged); zombie = eliminated by
    // resigning/flagging with the king still on the board.
    auto resigned = dict_tuple(dict, "resigned"), flagged = dict_tuple(dict, "flagged");
    auto zombies  = dict_tuple(dict, "zombie");
    for (int c = 0; c < 4; ++c) {
        bool rs  = (c < (int)resigned.size() && resigned[c] == "true") || (c < (int)flagged.size() && flagged[c] == "true");
        bool out = elim[c] || grayPiece[c] || rs;
        playerStatus[c] = ALIVE;
        if (out) playerStatus[c] = (rs && kingSq[c] != SQ_NONE) ? ZOMBIE : DEAD;
        if (c < (int)zombies.size() && !zombies[c].empty())
            playerStatus[c] = zombies[c] == "true" ? uint8_t(ZOMBIE) : playerStatus[c];
        if (playerStatus[c] == ALIVE && kingSq[c] == SQ_NONE) playerStatus[c] = DEAD;
    }

    auto eps = dict_tuple(dict, "enPassant");
    for (int c = 0; c < 4 && c < (int)eps.size(); ++c) {
        std::string t = eps[c];
        size_t      colon = t.find(':');
        if (colon != std::string::npos) t = t.substr(0, colon);
        if (!t.empty()) epSquares[c] = string_to_square(t);
    }

    for (int c = 0; c < 4; ++c) {
        if (kingSq[c] == SQ_NONE || playerStatus[c] == DEAD) {
            castling &= ~castling_rights_of(Color(c));
            continue;
        }
        Bitboard rooks = pieces(Color(c), ROOK);
        while (rooks) set_castling_right(Color(c), pop_lsb(rooks));
    }
    int valid = 0;
    for (int bit = 0; bit < 8; ++bit)
        if ((castling & (1 << bit)) && castlingRookSquare[bit] != SQ_NONE) valid |= 1 << bit;
    castling = valid;

    gamePly    = 0;
    endReason  = END_NONE;
    lastMover  = NO_COLOR;
    paidKings  = 0;
    lastEvents = MoveEvents();
    posKey     = compute_key();
    compute_checkers();
    if (playerStatus[sideToMove] == DEAD) {
        advance_turn();
    }
    // A FEN of a finished game already includes the end bonuses: mark it over without adding them again.
    int ptsBefore[COLOR_NB];
    for (int c = 0; c < COLOR_NB; ++c) ptsBefore[c] = pts[c];
    check_game_end();
    if (endReason != END_NONE)
        for (int c = 0; c < COLOR_NB; ++c) pts[c] = ptsBefore[c], endBonus[c] = 0;
    return true;
}

std::string Position::fen() const {
    std::ostringstream os;
    os << char(toupper(ColorChar[sideToMove])) << '-';
    // chess.com marks only dead players as eliminated; a zombie shows up in 'resigned'
    for (int c = 0; c < 4; ++c) os << (playerStatus[c] == DEAD ? 1 : 0) << (c < 3 ? "," : "-");
    for (int c = 0; c < 4; ++c) os << ((castling & castling_oo(Color(c))) ? 1 : 0) << (c < 3 ? "," : "-");
    for (int c = 0; c < 4; ++c) os << ((castling & castling_ooo(Color(c))) ? 1 : 0) << (c < 3 ? "," : "-");
    for (int c = 0; c < 4; ++c) os << pts[c] << (c < 3 ? "," : "-");
    os << rule50 << '-';

    bool anyEp = false, anyZombie = false;
    for (int c = 0; c < 4; ++c) {
        anyEp |= epSquares[c] != SQ_NONE;
        anyZombie |= playerStatus[c] == ZOMBIE;
    }
    if (anyEp || anyZombie) {
        os << '{';
        bool first = true;
        if (anyEp) {
            os << "'enPassant':(";
            for (int c = 0; c < 4; ++c) {
                if (epSquares[c] != SQ_NONE)
                    os << '\'' << square_to_string(epSquares[c]) << ':'
                       << square_to_string(epSquares[c] + pawn_push(Color(c))) << '\'';
                else
                    os << "''";
                if (c < 3) os << ',';
            }
            os << ')';
            first = false;
        }
        if (anyZombie) {
            if (!first) os << ',';
            os << "'resigned':(";
            for (int c = 0; c < 4; ++c) os << (playerStatus[c] == ZOMBIE ? "true" : "null") << (c < 3 ? "," : "");
            os << ')';
        }
        os << "}-";
    }

    for (int r = 14; r >= 1; --r) {
        int  empties = 0;
        bool firstCell = true;
        auto emit = [&](const std::string& cell) {
            if (!firstCell) os << ',';
            os << cell;
            firstCell = false;
        };
        for (int f = 1; f <= 14; ++f) {
            Square s = make_square(File(f), Rank(r));
            if (!is_ok(s) || piece_on(s) != NO_PIECE) {
                if (empties) emit(std::to_string(empties));
                empties = 0;
            }
            if (!is_ok(s)) {
                emit("x");
                continue;
            }
            Piece pc = piece_on(s);
            if (pc == NO_PIECE) {
                empties++;
                continue;
            }
            std::string cell;
            // gray pieces get a 'd' prefix; a zombie's (still moving) king doesn't
            PlayerStatus st = PlayerStatus(playerStatus[color_of(pc)]);
            if (st == DEAD || (st == ZOMBIE && type_of(pc) != KING)) cell += 'd';
            cell += ColorChar[color_of(pc)];
            cell += promotedBB.test(s) ? 'D' : " PNBRQK"[type_of(pc)];
            emit(cell);
        }
        if (empties) emit(std::to_string(empties));
        if (r > 1) os << '/';
    }
    return os.str();
}

std::string Position::pretty() const {
    std::ostringstream os;
    for (int r = 14; r >= 1; --r) {
        os << (r < 10 ? " " : "") << r << " ";
        for (int f = 1; f <= 14; ++f) {
            Square s = make_square(File(f), Rank(r));
            if (!is_ok(s)) {
                os << "   ";
                continue;
            }
            Piece pc = piece_on(s);
            if (pc == NO_PIECE) {
                os << " . ";
                continue;
            }
            bool gray = playerStatus[color_of(pc)] != ALIVE;
            os << (gray ? '(' : ' ') << ColorChar[color_of(pc)]
               << (promotedBB.test(s) ? 'D' : " PNBRQK"[type_of(pc)]);
        }
        os << "\n";
    }
    os << "    a  b  c  d  e  f  g  h  i  j  k  l  m  n\n";
    os << "Turn: " << ColorName[sideToMove] << "  Points:";
    for (int c = 0; c < 4; ++c)
        os << " " << ColorChar[c] << "=" << pts[c]
           << (playerStatus[c] == ALIVE ? "" : playerStatus[c] == ZOMBIE ? "(zombie)" : "(dead)");
    if (game_over()) os << "  Game over: " << end_reason_name(end_reason());
    os << "\n";
    return os.str();
}

// ------------------------------------------------------------------------------------------------
//  Attacks
// ------------------------------------------------------------------------------------------------

Bitboard Position::attacking_pieces() const {
    Bitboard b = Bitboard::zero();
    for (int c = 0; c < COLOR_NB; ++c) {
        if (playerStatus[c] == ALIVE)
            b |= byColorBB[c];
        else if (playerStatus[c] == ZOMBIE && Rules.zombieKingAttacks && kingSq[c] != SQ_NONE)
            b |= kingSq[c];
    }
    return b;
}

Bitboard Position::attackers_to(Square s, const Bitboard& occ) const {
    Bitboard r = (PseudoAttacks[KNIGHT][s] & byTypeBB[KNIGHT]) | (PseudoAttacks[KING][s] & byTypeBB[KING]);
    for (int c = 0; c < COLOR_NB; ++c) r |= PawnAttacks[c ^ 2][s] & pieces(Color(c), PAWN);
    Bitboard rq = byTypeBB[ROOK] | byTypeBB[QUEEN], bq = byTypeBB[BISHOP] | byTypeBB[QUEEN];
    if (PseudoAttacks[ROOK][s] & rq) r |= get_rook_attacks(s, occ) & rq;
    if (PseudoAttacks[BISHOP][s] & bq) r |= get_bishop_attacks(s, occ) & bq;
    return r;
}

bool Position::attacked_by_others(Square s, Color us, const Bitboard& occ) const {
    return bool(attackers_to(s, occ) & attacking_pieces() & ~byColorBB[us]);
}

void Position::compute_checkers() {
    Bitboard att = attacking_pieces();
    for (int c = 0; c < COLOR_NB; ++c) {
        if (playerStatus[c] == DEAD || kingSq[c] == SQ_NONE)
            checkersBB[c] = Bitboard::zero();
        else
            checkersBB[c] = attackers_to(kingSq[c], pieces()) & att & ~byColorBB[c];
    }
}

int Position::capture_value(Square s) const {
    Piece pc = piece_on(s);
    if (pc == NO_PIECE) return 0;
    Color c = color_of(pc);
    if (playerStatus[c] != ALIVE)
        return type_of(pc) != KING              ? Rules.grayValue
             : playerStatus[c] != ZOMBIE         ? Rules.grayKingValue
             : (paidKings & (1 << c))            ? 0
                                                 : Rules.zombieKingValue;
    if (promotedBB.test(s)) return Rules.promotedValue;
    return Rules.pieceValue[type_of(pc)];
}

// ------------------------------------------------------------------------------------------------
//  Move generation
// ------------------------------------------------------------------------------------------------

namespace {
constexpr Direction LeftCap[COLOR_NB]  = {NORTH_WEST, NORTH_EAST, SOUTH_EAST, SOUTH_WEST};
constexpr Direction RightCap[COLOR_NB] = {NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST};
}  // namespace

template<Color Us>
Move* Position::gen_pawn_moves(Move* list, const Bitboard& enemies, bool capturesOnly) const {
    constexpr Direction Up    = pawn_push(Us);
    constexpr Direction Left  = LeftCap[Us];
    constexpr Direction Right = RightCap[Us];

    const Bitboard pawns   = pieces(Us, PAWN);
    if (!pawns) return list;
    const Bitboard emptyBB = ~pieces() & LegalSquaresBB;
    const Bitboard promo   = RelRankBB[Us][Rules.promotionRelRank];

    Bitboard single = shift<Up>(pawns) & emptyBB;
    Bitboard promos = single & promo;
    while (promos) {
        Square to = pop_lsb(promos);
        *list++   = Move::make<PROMOTION>(to - Up, to, QUEEN);
    }
    if (!capturesOnly) {
        Bitboard quiet = single & ~promo;
        Bitboard dbl   = shift<Up>(single & RelRankBB[Us][2]) & emptyBB;
        while (quiet) {
            Square to = pop_lsb(quiet);
            *list++   = Move::make<NORMAL>(to - Up, to);
        }
        while (dbl) {
            Square to = pop_lsb(dbl);
            *list++   = Move::make<NORMAL>(to - Up - Up, to);
        }
    }

    Bitboard epBB = Bitboard::zero();
    for (int c = 0; c < COLOR_NB; ++c)
        if (c != Us && epSquares[c] != SQ_NONE) epBB |= epSquares[c];

    Bitboard capL = shift<Left>(pawns) & enemies & ~epBB;
    Bitboard capR = shift<Right>(pawns) & enemies & ~epBB;
    while (capL) {
        Square to = pop_lsb(capL);
        *list++   = promo.test(to) ? Move::make<PROMOTION>(to - Left, to, QUEEN) : Move::make<NORMAL>(to - Left, to);
    }
    while (capR) {
        Square to = pop_lsb(capR);
        *list++   = promo.test(to) ? Move::make<PROMOTION>(to - Right, to, QUEEN) : Move::make<NORMAL>(to - Right, to);
    }

    // En passant onto another color's skipped square. The square may also hold a piece, which is then
    // captured too.
    Bitboard done = Bitboard::zero();
    for (int c = 0; c < COLOR_NB; ++c) {
        Square ep = epSquares[c];
        if (c == Us || ep == SQ_NONE || done.test(ep) || byColorBB[Us].test(ep)) continue;
        done |= ep;
        PieceType pt = promo.test(ep) ? QUEEN : NO_PIECE_TYPE;
        Square    f1 = ep - Left, f2 = ep - Right;
        if (is_ok(f1) && pawns.test(f1)) *list++ = Move::make<EN_PASSANT>(f1, ep, pt);
        if (is_ok(f2) && pawns.test(f2)) *list++ = Move::make<EN_PASSANT>(f2, ep, pt);
    }
    return list;
}

Move* Position::gen_piece_moves(Color us, Move* list, const Bitboard& targets) const {
    const Bitboard occ = pieces();
    for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN}) {
        Bitboard b = pieces(us, pt);
        while (b) {
            Square   from = pop_lsb(b);
            Bitboard att  = attacks_bb(pt, from, occ) & targets;
            while (att) *list++ = Move(from, pop_lsb(att));
        }
    }
    return list;
}

Move* Position::gen_castling(Color us, Move* list) const {
    Square ksq = kingSq[us];
    if (ksq == SQ_NONE || checkersBB[us]) return list;
    for (int bit = 2 * us; bit <= 2 * us + 1; ++bit) {
        if (!(castling & (1 << bit))) continue;
        Square rsq = castlingRookSquare[bit];
        if (rsq == SQ_NONE || piece_on(rsq) != make_piece(us, ROOK)) continue;
        if (between_bb(ksq, rsq) & pieces()) continue;
        bool      alongRank = (us == RED || us == YELLOW);
        Direction d   = alongRank ? (rsq > ksq ? EAST : WEST) : (rsq > ksq ? NORTH : SOUTH);
        Square    mid = ksq + d, kto = ksq + d + d;
        Bitboard  occ = pieces() ^ ksq;
        if (attacked_by_others(mid, us, occ) || attacked_by_others(kto, us, occ)) continue;
        *list++ = Move::make<CASTLING>(ksq, kto);
    }
    return list;
}

bool Position::legal_for(Color us, Move m) const {
    Square ksq = kingSq[us];
    if (ksq == SQ_NONE || m.type_of() == CASTLING) return true;
    const Square from = m.from_sq(), to = m.to_sq();
    const Bitboard att = attacking_pieces() & ~byColorBB[us];

    if (from == ksq) {
        Bitboard occ = (pieces() ^ from) | to;
        return !(attackers_to(to, occ) & att & ~square_bb(to));
    }
    Bitboard removed = square_bb(to);
    Bitboard occ     = (pieces() ^ from) | to;
    if (m.type_of() == EN_PASSANT) {
        for (int c = 0; c < COLOR_NB; ++c)
            if (c != us && epSquares[c] == to) {
                Square v = to + pawn_push(Color(c));
                if (is_ok(v) && type_of(piece_on(v)) == PAWN && color_of(piece_on(v)) == c) {
                    removed |= v;
                    occ ^= v;
                }
                break;
            }
    } else if (!checkersBB[us] && !PseudoAttacks[QUEEN][ksq].test(from))
        return true;
    return !(attackers_to(ksq, occ) & att & ~removed);
}

int Position::generate_legal_for(Color us, Move* list, bool capturesOnly) const {
    if (playerStatus[us] == DEAD || endReason != END_NONE) return 0;
    Move  buf[MAX_MOVES];
    Move* end = buf;
    const Bitboard enemies = pieces() & ~byColorBB[us];
    const Bitboard targets = capturesOnly ? enemies : (LegalSquaresBB & ~byColorBB[us]);
    Square ksq = kingSq[us];

    if (playerStatus[us] == ALIVE) {
        switch (us) {
        case RED: end = gen_pawn_moves<RED>(end, enemies, capturesOnly); break;
        case BLUE: end = gen_pawn_moves<BLUE>(end, enemies, capturesOnly); break;
        case YELLOW: end = gen_pawn_moves<YELLOW>(end, enemies, capturesOnly); break;
        default: end = gen_pawn_moves<GREEN>(end, enemies, capturesOnly); break;
        }
        end = gen_piece_moves(us, end, targets);
    }
    if (!capturesOnly && playerStatus[us] != DEAD) end = gen_castling(us, end);
    if (ksq != SQ_NONE) {
        // A zombie king may also capture its own (gray) pieces.
        Bitboard kt = playerStatus[us] == ZOMBIE ? (capturesOnly ? pieces() : LegalSquaresBB) : targets;
        Bitboard b  = PseudoAttacks[KING][ksq] & kt;
        while (b) *end++ = Move(ksq, pop_lsb(b));
    }

    int n = 0;
    for (Move* m = buf; m < end; ++m)
        if (legal_for(us, *m)) list[n++] = *m;
    return n;
}

// Stops at the first legal move: king moves first, then everything else.
bool Position::has_legal_move(Color us) const {
    if (playerStatus[us] == DEAD || endReason != END_NONE) return false;
    const Square ksq = kingSq[us];
    if (ksq != SQ_NONE) {
        Bitboard kt = playerStatus[us] == ZOMBIE ? LegalSquaresBB : (LegalSquaresBB & ~byColorBB[us]);
        Bitboard b  = PseudoAttacks[KING][ksq] & kt;
        while (b)
            if (legal_for(us, Move(ksq, pop_lsb(b)))) return true;
    }
    Move  buf[MAX_MOVES];
    Move* end = buf;
    if (playerStatus[us] == ALIVE) {
        const Bitboard enemies = pieces() & ~byColorBB[us];
        end = gen_piece_moves(us, end, LegalSquaresBB & ~byColorBB[us]);
        switch (us) {
        case RED: end = gen_pawn_moves<RED>(end, enemies, false); break;
        case BLUE: end = gen_pawn_moves<BLUE>(end, enemies, false); break;
        case YELLOW: end = gen_pawn_moves<YELLOW>(end, enemies, false); break;
        default: end = gen_pawn_moves<GREEN>(end, enemies, false); break;
        }
    }
    end = gen_castling(us, end);
    for (Move* m = buf; m < end; ++m)
        if (legal_for(us, *m)) return true;
    return false;
}

bool Position::is_legal_move(Move m) const {
    Move list[MAX_MOVES];
    int  n = generate_legal(list);
    for (int i = 0; i < n; ++i)
        if (list[i] == m) return true;
    return false;
}

Move Position::parse_move(const std::string& sIn) const {
    std::string s = sIn;
    for (auto& ch : s) ch = char(tolower((unsigned char)ch));
    if (!s.empty() && (s.back() == 'q' || s.back() == 'd')) s.back() = 'd';
    Move list[MAX_MOVES];
    int  n = generate_legal(list);
    for (int i = 0; i < n; ++i)
        if (move_to_string(list[i]) == s) return list[i];
    // a promotion given without its piece letter
    for (int i = 0; i < n; ++i)
        if (list[i].is_promotion() && move_to_string(list[i]) == s + "d") return list[i];
    return Move::none();
}

// ------------------------------------------------------------------------------------------------
//  Making moves
// ------------------------------------------------------------------------------------------------

void Position::eliminate(Color c, PlayerStatus newStatus) {
    if (playerStatus[c] == newStatus) return;
    posKey ^= Zobrist::status[c][playerStatus[c]] ^ Zobrist::status[c][newStatus];
    playerStatus[c] = uint8_t(newStatus);
    if (newStatus != DEAD) return;  // a zombie king can still castle (as its random move)
    int cr = castling & ~castling_rights_of(c);
    if (cr != castling) {
        posKey ^= Zobrist::castling[castling] ^ Zobrist::castling[cr];
        castling = cr;
    }
}

void Position::do_move(Move m) {
    assert(endReason == END_NONE);
    lastEvents = MoveEvents();
    const Color    us   = sideToMove;
    const Square   from = m.from_sq(), to = m.to_sq();
    const Piece    pc   = piece_on(from);
    const MoveType type = m.type_of();
    Bitboard prevCheckers[COLOR_NB];
    for (int c = 0; c < COLOR_NB; ++c) prevCheckers[c] = checkersBB[c];
    Square epPrev[COLOR_NB];
    for (int c = 0; c < COLOR_NB; ++c) epPrev[c] = epSquares[c];

    gamePly++;
    if (playerStatus[us] == ALIVE) rule50++;
    posKey ^= Zobrist::castling[castling];
    for (int c = 0; c < COLOR_NB; ++c)
        if (epSquares[c] != SQ_NONE) posKey ^= Zobrist::enpassant[epSquares[c]];

    // En passant targets: ours expires; a target dies when its pawn moves or is captured.
    epSquares[us] = SQ_NONE;
    for (int c = 0; c < COLOR_NB; ++c) {
        if (epSquares[c] == SQ_NONE) continue;
        Square v = epSquares[c] + pawn_push(Color(c));
        if (to == v || from == v || (type == EN_PASSANT && to == epSquares[c])) epSquares[c] = SQ_NONE;
    }

    int     gained   = 0;
    uint8_t kingCaps = 0;
    auto capture_at = [&](Square s) {
        Piece cp = piece_on(s);
        gained += capture_value(s);
        if (type_of(cp) == KING && playerStatus[color_of(cp)] != DEAD) kingCaps |= uint8_t(1 << color_of(cp));
        posKey ^= Zobrist::psq[cp][s];
        if (promotedBB.test(s)) posKey ^= Zobrist::promoted[s];
        if (playerStatus[color_of(cp)] == ALIVE) rule50 = 0;  // capturing gray pieces doesn't reset it
        remove_piece(s);
    };

    if (type == CASTLING) {
        bool   alongRank = (us == RED || us == YELLOW);
        Square rfrom     = SQ_NONE;
        for (int bit = 2 * us; bit <= 2 * us + 1; ++bit) {
            Square r = castlingRookSquare[bit];
            if (r == SQ_NONE) continue;
            bool sameSide = alongRank ? ((file_of(r) > file_of(from)) == (file_of(to) > file_of(from)))
                                      : ((rank_of(r) > rank_of(from)) == (rank_of(to) > rank_of(from)));
            if (sameSide) rfrom = r;
        }
        Square rto  = Square((from + to) / 2);
        Piece  rook = piece_on(rfrom);
        posKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to] ^ Zobrist::psq[rook][rfrom] ^ Zobrist::psq[rook][rto];
        move_piece(from, to);
        move_piece(rfrom, rto);
    } else {
        if (piece_on(to) != NO_PIECE) capture_at(to);
        if (type == EN_PASSANT) {
            for (int c = 0; c < COLOR_NB; ++c)
                if (c != us && epPrev[c] == to) {
                    Square v = to + pawn_push(Color(c));
                    if (is_ok(v) && piece_on(v) == make_piece(Color(c), PAWN)) capture_at(v);
                    break;
                }
        }
        bool wasPromoted = promotedBB.test(from);
        posKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
        if (wasPromoted) posKey ^= Zobrist::promoted[from] ^ Zobrist::promoted[to];
        move_piece(from, to);
        if (type_of(pc) == PAWN) {
            rule50 = 0;
            if (to == from + pawn_push(us) + pawn_push(us)) epSquares[us] = from + pawn_push(us);
            if (m.is_promotion()) {
                remove_piece(to);
                posKey ^= Zobrist::psq[pc][to];
                Piece q = make_piece(us, m.promotion_type());
                put_piece(q, to);
                promotedBB |= to;
                posKey ^= Zobrist::psq[q][to] ^ Zobrist::promoted[to];
            }
        }
    }

    castling &= ~(castlingRightsMask[from] | castlingRightsMask[to]);
    posKey ^= Zobrist::castling[castling];
    for (int c = 0; c < COLOR_NB; ++c)
        if (epSquares[c] != SQ_NONE) posKey ^= Zobrist::enpassant[epSquares[c]];

    const bool scores = playerStatus[us] == ALIVE || Rules.zombieScores;
    if (scores) add_points(us, gained);
    lastEvents.captureValue = gained;
    lastEvents.kingCapMask  = kingCaps;
    for (int c = 0; c < COLOR_NB; ++c)
        if (kingCaps & (1 << c)) eliminate(Color(c), DEAD);

    compute_checkers();

    // Check bonus: kings attacked from a square they weren't attacked from before the move.
    // Zombie kings get checked (and annotated) too, but don't count for the bonus.
    int nChecked = 0;
    for (int c = 0; c < COLOR_NB; ++c) {
        if (c == us || playerStatus[c] == DEAD || kingSq[c] == SQ_NONE) continue;
        // a piece that captured a checker on its square is a new checker too
        if (checkersBB[c] & ~(prevCheckers[c] & ~square_bb(to))) {
            lastEvents.checkedMask |= uint8_t(1 << c);
            if (playerStatus[c] == ALIVE || Rules.zombiesCountForChecks) nChecked++;
        }
    }
    int bonus = nChecked == 2 ? Rules.checkBonus2 : nChecked >= 3 ? Rules.checkBonus3 : 0;
    if (scores) add_points(us, bonus);
    lastEvents.checkBonus = bonus;

    // Immediate checkmates of every player (alive or zombie) in check without a legal move, credited to
    // the mover.
    uint8_t mated = 0;
    for (int c = 0; c < COLOR_NB; ++c) {
        if (c == us || playerStatus[c] == DEAD || !checkersBB[c]) continue;
        if (!has_legal_move(Color(c))) mated |= uint8_t(1 << c);
    }
    lastMover = us;
    if (mated) {
        for (int c = 0; c < COLOR_NB; ++c)
            if (mated & (1 << c)) {
                if (scores && !(paidKings & (1 << c))) add_points(us, Rules.mateBonus);
                eliminate(Color(c), DEAD);
            }
        compute_checkers();
    }
    lastEvents.matedMask = mated;

    if (alive_count() <= 1) {
        check_game_end();
        return;
    }
    advance_turn();
    check_game_end();
}

void Position::do_pass() {
    lastEvents = MoveEvents();
    gamePly++;
    rule50++;
    if (epSquares[sideToMove] != SQ_NONE) {
        posKey ^= Zobrist::enpassant[epSquares[sideToMove]];
        epSquares[sideToMove] = SQ_NONE;
    }
    advance_turn();
    check_game_end();
}

void Position::resign(Color c, bool passTurn) {
    if (playerStatus[c] != ALIVE || endReason != END_NONE) return;
    lastEvents = MoveEvents();
    if (checkersBB[c] && passTurn) {
        // Resigning or flagging in check: the player giving check gets the king's 20 points at once.
        // The king still walks as a zombie, but is worth nothing more.
        Color checker = color_of(piece_on(lsb(checkersBB[c])));
        if (playerStatus[checker] == ALIVE) add_points(checker, Rules.mateBonus);
        lastEvents.turnMateMask |= uint8_t(1 << c);
        paidKings |= uint8_t(1 << c);
    }
    eliminate(c, kingSq[c] != SQ_NONE ? ZOMBIE : DEAD);
    compute_checkers();
    if (alive_count() <= 1) {
        check_game_end();
        return;
    }
    if (c == sideToMove && passTurn) {
        gamePly++;
        advance_turn();
    } else if (c == sideToMove && !has_legal_move(c))
        advance_turn();  // the zombie king can't move: stalemated at once
    check_game_end();
}

// Passes the turn to the next player that isn't dead. A player without legal moves at the start of their
// turn is stalemated (or, if in check, mated without a mating move) and drops out.
void Position::advance_turn() {
    Color c = sideToMove;
    posKey ^= Zobrist::side[sideToMove];
    for (int i = 0; i < 8; ++i) {
        c = next_color(c);
        if (playerStatus[c] == DEAD) continue;
        if (alive_count() <= 1) break;
        if (!has_legal_move(c)) {
            if (!checkersBB[c]) {
                lastEvents.stalemateMask |= uint8_t(1 << c);
                if (playerStatus[c] == ALIVE)
                    add_points(c, Rules.stalemateBonus);
                else if (int n = alive_count(); n && !(paidKings & (1 << c))) {
                    int share = (Rules.zombieStalemateSplit + n - 1) / n;
                    for (int o = 0; o < COLOR_NB; ++o)
                        if (playerStatus[o] == ALIVE) add_points(Color(o), share);
                }
            } else {
                // mated without a mating move: the points go to the player giving check
                lastEvents.turnMateMask |= uint8_t(1 << c);
                Color checker = color_of(piece_on(lsb(checkersBB[c])));
                if (playerStatus[checker] == ALIVE && !(paidKings & (1 << c))) add_points(checker, Rules.mateBonus);
            }
            eliminate(c, DEAD);
            compute_checkers();
            continue;
        }
        break;
    }
    sideToMove = c;
    posKey ^= Zobrist::side[sideToMove];
}

bool Position::insufficient_material() const {
    // Two alive players with only their kings and at most one minor piece between them, or three or four
    // alive players with bare kings.
    const int alive = alive_count();
    if (alive < 2) return false;
    int minors = 0;
    for (int c = 0; c < COLOR_NB; ++c) {
        if (playerStatus[c] != ALIVE) continue;
        Bitboard mine = byColorBB[c];
        if (mine & (byTypeBB[PAWN] | byTypeBB[ROOK] | byTypeBB[QUEEN])) return false;
        minors += popcount(mine & (byTypeBB[KNIGHT] | byTypeBB[BISHOP]));
    }
    return minors <= (alive == 2 ? 1 : 0);
}

// "Claiming the win" on chess.com is resigning while far enough ahead: with two players alive, the other one
// becomes the last player and gets 20 for our king and for every zombie king left. True if resigning now
// leaves c strictly first.
bool Position::resign_wins(Color c) const {
    if (endReason != END_NONE || playerStatus[c] != ALIVE || alive_count() != 2) return false;
    Position p = *this;
    p.resign(c);
    if (!p.game_over()) return false;
    for (int o = 0; o < COLOR_NB; ++o)
        if (o != c && p.pts[o] >= p.pts[c]) return false;
    return true;
}

void Position::end_drawn(EndReason reason) { finish_game(reason); }

void Position::reopen() {
    if (endReason == END_NONE) return;
    for (int c = 0; c < COLOR_NB; ++c) {
        pts[c] -= endBonus[c];
        endBonus[c] = 0;
    }
    endReason = END_NONE;
    if (playerStatus[sideToMove] == DEAD || !has_legal_move(sideToMove)) advance_turn();
}

void Position::check_game_end() {
    if (endReason != END_NONE) return;
    if (alive_count() <= 1) finish_game(END_LAST_PLAYER);
    if (endReason == END_NONE && insufficient_material()) finish_game(END_INSUFFICIENT);
    if (endReason == END_NONE && Rules.rule50Moves > 0) {
        int movers = alive_count();  // 50 moves by every alive player (zombie moves don't count)
        if (rule50 >= Rules.rule50Moves * movers) finish_game(END_RULE50);
    }
}

// Final bonuses. The winner (the last alive player) gets endBonusPerKing for every zombie king still on the
// board (a claim is a resignation, so the claimer's own king counts for the winner). In a drawn ending each of the n alive
// players gets drawBonus * (n - 1) / n; leftover zombie kings go to the player who made the last move after
// insufficient material, and are shared by the alive players after a repetition or the 50-move rule.
void Position::finish_game(EndReason reason) {
    endReason = uint8_t(reason);
    int before[COLOR_NB];
    for (int c = 0; c < COLOR_NB; ++c) before[c] = pts[c];
    int leftoverKings = 0;
    for (int c = 0; c < COLOR_NB; ++c) {
        if (kingSq[c] == SQ_NONE || playerStatus[c] == ALIVE) continue;
        if (paidKings & (1 << c)) continue;
        if (playerStatus[c] == ZOMBIE || Rules.endBonusCountsDeadKings) leftoverKings++;
    }
    Color winner = NO_COLOR;
    if (reason == END_LAST_PLAYER) {
        for (int c = 0; c < COLOR_NB; ++c)
            if (playerStatus[c] == ALIVE) winner = Color(c);
    } else {
        int n = alive_count();
        for (int c = 0; c < COLOR_NB; ++c)
            if (playerStatus[c] == ALIVE) add_points(Color(c), n ? Rules.drawBonus * (n - 1) / n : 0);
        if (reason == END_INSUFFICIENT && n == 2 && lastMover != NO_COLOR && playerStatus[lastMover] == ALIVE)
            winner = lastMover;
        else if (n && leftoverKings) {
            int share = (Rules.endBonusPerKing * leftoverKings + n - 1) / n;
            for (int c = 0; c < COLOR_NB; ++c)
                if (playerStatus[c] == ALIVE) add_points(Color(c), share);
            leftoverKings = 0;
        }
    }
    if (winner != NO_COLOR) add_points(winner, Rules.endBonusPerKing * leftoverKings);
    for (int c = 0; c < COLOR_NB; ++c) endBonus[c] = pts[c] - before[c];
}

// ------------------------------------------------------------------------------------------------
//  Game
// ------------------------------------------------------------------------------------------------

int Game::repetitions() const {
    int n = 0;
    Key k = pos.board_key();
    for (size_t i = 0; i + 1 < history.size(); ++i) n += history[i] == k;
    return n;
}

void Game::play(Move m) {
    pos.do_move(m);
    history.push_back(pos.board_key());
    if (!pos.game_over() && repetitions() >= 2) pos.end_drawn(END_REPETITION);
}

void Game::pass_turn() {
    pos.do_pass();
    history.push_back(pos.board_key());
}

}  // namespace FFA
