/*
  4PC FFA engine - basic types.
  The square layout, bitboard type and Move encoding are taken from stockfish_4pc (GPLv3, derived from
  Stockfish); the team logic is removed and FFA player states are added.
*/

#ifndef TYPES_H_INCLUDED
#define TYPES_H_INCLUDED

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>

#include "FastUint256.h"

// Native PEXT is x86 BMI2-only; other targets use computed attacks.
#if defined(USE_PEXT) && defined(__BMI2__) && (defined(__i386__) || defined(__x86_64__))
    #include <immintrin.h>
    #define pext(b, m) _pext_u64(b, m)
    #define pdep(b, m) _pdep_u64(b, m)
    #define FFA_HAS_NATIVE_PEXT 1
#else
    #define FFA_HAS_NATIVE_PEXT 0
inline uint64_t pext(uint64_t b, uint64_t m) {
    uint64_t res = 0;
    for (uint64_t bb = 1; m; bb <<= 1) {
        uint64_t lowest = m & -m;
        if (b & lowest) res |= bb;
        m &= m - 1;
    }
    return res;
}
inline uint64_t pdep(uint64_t b, uint64_t m) {
    uint64_t res = 0;
    for (uint64_t bb = 1; m; bb <<= 1) {
        uint64_t lowest = m & -m;
        if (b & bb) res |= lowest;
        m &= m - 1;
    }
    return res;
}
#endif

namespace FFA {

constexpr bool HasPext = FFA_HAS_NATIVE_PEXT;

using Bitboard = my_math::FastUint256;
using Key      = uint64_t;
using Depth    = int;
using Value    = int;

constexpr int MAX_MOVES = 512;
constexpr int MAX_PLY   = 128;

// ------------------------------------------------------------------------------------------------
//  Players
// ------------------------------------------------------------------------------------------------

enum Color : int { RED, BLUE, YELLOW, GREEN, COLOR_NB = 4, NO_COLOR = 4 };

// ALIVE players move and attack. A ZOMBIE (resigned or flagged, "DeadKingWalking") has a gray army and a
// king that makes random legal king moves. A DEAD player (checkmated, stalemated, king captured) has a gray
// army and never moves again. Gray pieces block, can be captured, and never attack.
enum PlayerStatus : uint8_t { ALIVE, ZOMBIE, DEAD };

constexpr Color next_color(Color c) { return Color((int(c) + 1) & 3); }

// ------------------------------------------------------------------------------------------------
//  Castling
// ------------------------------------------------------------------------------------------------

enum CastlingRights : int {
    NO_CASTLING,
    RED_OO = 1 << 0, RED_OOO = 1 << 1,
    BLUE_OO = 1 << 2, BLUE_OOO = 1 << 3,
    YELLOW_OO = 1 << 4, YELLOW_OOO = 1 << 5,
    GREEN_OO = 1 << 6, GREEN_OOO = 1 << 7,
    ANY_CASTLING = 255,
    CASTLING_RIGHT_NB = 256
};

constexpr int castling_oo(Color c) { return 1 << (2 * int(c)); }
constexpr int castling_ooo(Color c) { return 1 << (2 * int(c) + 1); }
constexpr int castling_rights_of(Color c) { return 3 << (2 * int(c)); }

// ------------------------------------------------------------------------------------------------
//  Search values
// ------------------------------------------------------------------------------------------------

enum Bound : uint8_t { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT = BOUND_UPPER | BOUND_LOWER };

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_INFINITE = 1000000;
constexpr Value VALUE_NONE     = 1000001;

// ------------------------------------------------------------------------------------------------
//  Pieces
// ------------------------------------------------------------------------------------------------

enum PieceType : int {
    NO_PIECE_TYPE, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    ALL_PIECES = 0,
    PIECE_TYPE_NB = 8
};

// Color (2 bits) << 3 | PieceType (3 bits)
enum Piece : int {
    NO_PIECE,
    PIECE_NB = 32
};

constexpr Piece     make_piece(Color c, PieceType pt) { return Piece((c << 3) + pt); }
constexpr PieceType type_of(Piece pc) { return PieceType(pc & 7); }
constexpr Color     color_of(Piece pc) { return Color(pc >> 3); }

// ------------------------------------------------------------------------------------------------
//  Squares: 16x16 grid, the 14x14 board in files/ranks 1..14, minus the 3x3 corners.
//  Index = rank * 16 + file.
// ------------------------------------------------------------------------------------------------

enum Square : int { SQUARE_ZERO = 0, SQ_NONE = 256, SQUARE_NB = 256 };

enum Direction : int {
    NORTH = 16, EAST = 1, SOUTH = -NORTH, WEST = -EAST,
    NORTH_EAST = NORTH + EAST, SOUTH_EAST = SOUTH + EAST,
    SOUTH_WEST = SOUTH + WEST, NORTH_WEST = NORTH + WEST
};

enum File : int {
    FILE_A = 1, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G,
    FILE_H, FILE_I, FILE_J, FILE_K, FILE_L, FILE_M, FILE_N,
    FILE_NB = 16
};

enum Rank : int {
    RANK_1 = 1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7,
    RANK_8, RANK_9, RANK_10, RANK_11, RANK_12, RANK_13, RANK_14,
    RANK_NB = 16
};

#define ENABLE_INCR_OPERATORS_ON(T) \
    constexpr T& operator++(T& d) { return d = T(int(d) + 1); } \
    constexpr T& operator--(T& d) { return d = T(int(d) - 1); }

ENABLE_INCR_OPERATORS_ON(PieceType)
ENABLE_INCR_OPERATORS_ON(Square)
ENABLE_INCR_OPERATORS_ON(File)
ENABLE_INCR_OPERATORS_ON(Rank)

#undef ENABLE_INCR_OPERATORS_ON

constexpr Direction operator+(Direction d1, Direction d2) { return Direction(int(d1) + int(d2)); }
constexpr Direction operator*(int i, Direction d) { return Direction(i * int(d)); }
constexpr Square    operator+(Square s, Direction d) { return Square(int(s) + int(d)); }
constexpr Square    operator-(Square s, Direction d) { return Square(int(s) - int(d)); }
constexpr Square&   operator+=(Square& s, Direction d) { return s = s + d; }
constexpr Square&   operator-=(Square& s, Direction d) { return s = s - d; }

constexpr uint64_t IsOkMask[4] = {
    0x0FF00FF00FF00000ULL,  // ranks 0-3
    0x7FFE7FFE7FFE7FFEULL,  // ranks 4-7
    0x7FFE7FFE7FFE7FFEULL,  // ranks 8-11
    0x00000FF00FF00FF0ULL   // ranks 12-15
};

constexpr bool is_ok(Square s) {
    return static_cast<unsigned>(s) < 256 && (IsOkMask[s >> 6] & (1ULL << (s & 63)));
}

constexpr File   file_of(Square s) { return File(s & 0xF); }
constexpr Rank   rank_of(Square s) { return Rank(s >> 4); }
constexpr Square make_square(File f, Rank r) { return Square((r << 4) + f); }

// Red moves north, Blue east, Yellow south, Green west.
constexpr Direction pawn_push(Color c) {
    return c == RED ? NORTH : c == BLUE ? EAST : c == YELLOW ? SOUTH : WEST;
}

// Distance (0..13) of s from c's back rank.
constexpr int relative_rank(Color c, Square s) {
    return c == RED    ? rank_of(s) - 1
         : c == YELLOW ? 14 - rank_of(s)
         : c == BLUE   ? file_of(s) - 1
                       : 14 - file_of(s);
}

// ------------------------------------------------------------------------------------------------
//  Moves (32 bits): 0-7 to, 8-15 from, 16-19 promotion piece type, 20-21 move type
// ------------------------------------------------------------------------------------------------

enum MoveType { NORMAL, PROMOTION = 1 << 20, EN_PASSANT = 2 << 20, CASTLING = 3 << 20 };

class Move {
   public:
    Move() = default;
    constexpr explicit Move(uint32_t d) : data(d) {}
    constexpr Move(Square from, Square to) : data((from << 8) | to) {}

    template<MoveType T>
    static constexpr Move make(Square from, Square to, PieceType pt = NO_PIECE_TYPE) {
        return Move(T + (pt << 16) + (from << 8) + to);
    }

    constexpr Square    from_sq() const { return Square((data >> 8) & 0xFF); }
    constexpr Square    to_sq() const { return Square(data & 0xFF); }
    constexpr MoveType  type_of() const { return MoveType(data & (3 << 20)); }
    constexpr PieceType promotion_type() const { return PieceType((data >> 16) & 0xF); }
    constexpr bool      is_promotion() const { return (data >> 16) & 0xF; }

    constexpr bool is_ok() const { return none().data != data && null().data != data; }

    static constexpr Move null() { return Move(257); }
    static constexpr Move none() { return Move(0); }

    constexpr bool     operator==(const Move& m) const { return data == m.data; }
    constexpr bool     operator!=(const Move& m) const { return data != m.data; }
    constexpr explicit operator bool() const { return data != 0; }
    constexpr uint32_t raw() const { return data; }

   protected:
    uint32_t data;
};

std::string square_to_string(Square s);
Square      string_to_square(const std::string& s);  // "a4" -> square, SQ_NONE if invalid
std::string move_to_string(Move m);                 // "d2d4", promotions "g7g8d"

constexpr char ColorChar[COLOR_NB] = {'r', 'b', 'y', 'g'};
constexpr const char* ColorName[COLOR_NB] = {"Red", "Blue", "Yellow", "Green"};

}  // namespace FFA

#endif  // TYPES_H_INCLUDED
