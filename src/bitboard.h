/*
  Stockfish 4PC, a 4-Player Chess Engine derived from Stockfish
  Bitboard Header
*/

#ifndef BITBOARD_H_INCLUDED
#define BITBOARD_H_INCLUDED

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>
#include <string>

#include "types.h"
#include "FastUint256.h"

namespace FFA {

namespace Bitboards {

    void init();
    std::string pretty(const Bitboard& b);

} // namespace Bitboards

// ------------------------------
//  Global Bitboard Constants
// ------------------------------

// 16-wide grid specific masks (borders vs active board)
// Note: Actual legal squares are defined by LegalSquaresBB
extern Bitboard LegalSquaresBB;
extern Bitboard File0BB;  // First File (A/0) - used for wrapping checks
extern Bitboard File15BB; // Last File (P/15) - used for wrapping checks

// Lookup tables
extern uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
extern Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];

// ------------------------------
//  Basic Operations
// ------------------------------

constexpr std::array<Bitboard, SQUARE_NB> make_square_bbs() {
    std::array<Bitboard, SQUARE_NB> bbs{};
    for (int s = 0; s < SQUARE_NB; ++s)
        bbs[s].set_bit(s);
    return bbs;
}

inline constexpr std::array<Bitboard, SQUARE_NB> SquareBBs = make_square_bbs();

// A table, so that the bitboard is one load. A bitboard built in memory one
// limb at a time can't be forwarded to the vector load that reads it next.
constexpr Bitboard square_bb(Square s) { return SquareBBs[s]; }

inline Bitboard  operator&(const Bitboard& b, Square s) { return b & square_bb(s); }
inline Bitboard  operator|(const Bitboard& b, Square s) { return b | square_bb(s); }
inline Bitboard  operator^(const Bitboard& b, Square s) { return b ^ square_bb(s); }
inline Bitboard& operator|=(Bitboard& b, Square s) { return b |= square_bb(s); }
inline Bitboard& operator^=(Bitboard& b, Square s) { return b ^= square_bb(s); }

inline Bitboard operator&(Square s, const Bitboard& b) { return b & s; }
inline Bitboard operator|(Square s, const Bitboard& b) { return b | s; }
inline Bitboard operator^(Square s, const Bitboard& b) { return b ^ s; }

inline Bitboard operator|(Square s1, Square s2) { return square_bb(s1) | s2; }

inline bool more_than_one(const Bitboard& b) { 
    // If more than one 64-bit limb has data, we definitely have >1 bits.
    int populated_limbs = (b.limbs[0] != 0) + (b.limbs[1] != 0) + (b.limbs[2] != 0) + (b.limbs[3] != 0);
    if (populated_limbs > 1) return true;
    
    // Otherwise, squash them together and use the standard bit-clearing trick
    uint64_t l = b.limbs[0] | b.limbs[1] | b.limbs[2] | b.limbs[3];
    return (l & (l - 1)) != 0;
}

// ------------------------------
//  Bit Manipulation Wrappers
// ------------------------------

inline int popcount(const Bitboard& b) { return b.popcount(); }

inline Square lsb(const Bitboard& b) {
    assert(b);
    return Square(b.ctz());
}

inline Square msb(const Bitboard& b) {
    assert(b);
    return Square(255 - b.clz()); // 256-bit specific
}

inline Bitboard least_significant_square_bb(const Bitboard& b) {
    assert(b);
    return square_bb(lsb(b));
}

inline Square pop_lsb(Bitboard& b) {
    assert(b);
#if defined(__BMI__) || defined(__GNUC__)
    // Instantly find and clear the lowest bit without recalculating indices
    if (b.limbs[0]) { int s = __builtin_ctzll(b.limbs[0]); b.limbs[0] &= b.limbs[0] - 1; return Square(s); }
    if (b.limbs[1]) { int s = __builtin_ctzll(b.limbs[1]); b.limbs[1] &= b.limbs[1] - 1; return Square(s + 64); }
    if (b.limbs[2]) { int s = __builtin_ctzll(b.limbs[2]); b.limbs[2] &= b.limbs[2] - 1; return Square(s + 128); }
    if (b.limbs[3]) { int s = __builtin_ctzll(b.limbs[3]); b.limbs[3] &= b.limbs[3] - 1; return Square(s + 192); }
#else
    const Square s = lsb(b);
    b.clear_bit(s);
    return s;
#endif
    return SQ_NONE;
}

// ------------------------------
//  Geometry & Distance
// ------------------------------

inline int distance(Square x, Square y) {
    return SquareDistance[x][y];
}

inline Bitboard line_bb(Square s1, Square s2) {
    return LineBB[s1][s2];
}

inline Bitboard between_bb(Square s1, Square s2) {
    return BetweenBB[s1][s2];
}

// Moves a bitboard one step along direction D.
// 4PC Note: We must mask files to prevent wrapping around the 16x16 grid
// (e.g. moving East from File 15 to File 0 of next rank).
template<Direction D>
inline Bitboard shift(const Bitboard& b) {
    Bitboard res = Bitboard::zero();
    
    if constexpr (D == NORTH) { // << 16
        res.limbs[0] = b.limbs[0] << 16;
        res.limbs[1] = (b.limbs[1] << 16) | (b.limbs[0] >> 48);
        res.limbs[2] = (b.limbs[2] << 16) | (b.limbs[1] >> 48);
        res.limbs[3] = (b.limbs[3] << 16) | (b.limbs[2] >> 48);
        return res;
    }
    if constexpr (D == SOUTH) { // >> 16
        res.limbs[0] = (b.limbs[0] >> 16) | (b.limbs[1] << 48);
        res.limbs[1] = (b.limbs[1] >> 16) | (b.limbs[2] << 48);
        res.limbs[2] = (b.limbs[2] >> 16) | (b.limbs[3] << 48);
        res.limbs[3] = b.limbs[3] >> 16;
        return res;
    }
    if constexpr (D == NORTH + NORTH) { // << 32
        res.limbs[0] = b.limbs[0] << 32;
        res.limbs[1] = (b.limbs[1] << 32) | (b.limbs[0] >> 32);
        res.limbs[2] = (b.limbs[2] << 32) | (b.limbs[1] >> 32);
        res.limbs[3] = (b.limbs[3] << 32) | (b.limbs[2] >> 32);
        return res;
    }
    if constexpr (D == SOUTH + SOUTH) { // >> 32
        res.limbs[0] = (b.limbs[0] >> 32) | (b.limbs[1] << 32);
        res.limbs[1] = (b.limbs[1] >> 32) | (b.limbs[2] << 32);
        res.limbs[2] = (b.limbs[2] >> 32) | (b.limbs[3] << 32);
        res.limbs[3] = b.limbs[3] >> 32;
        return res;
    }

    // East/West movements require masking the board edges to prevent wrapping
    Bitboard m;
    if constexpr (D == EAST) { // << 1
        m = b & ~File15BB;
        res.limbs[0] = m.limbs[0] << 1;
        res.limbs[1] = (m.limbs[1] << 1) | (m.limbs[0] >> 63);
        res.limbs[2] = (m.limbs[2] << 1) | (m.limbs[1] >> 63);
        res.limbs[3] = (m.limbs[3] << 1) | (m.limbs[2] >> 63);
        return res;
    }
    if constexpr (D == WEST) { // >> 1
        m = b & ~File0BB;
        res.limbs[0] = (m.limbs[0] >> 1) | (m.limbs[1] << 63);
        res.limbs[1] = (m.limbs[1] >> 1) | (m.limbs[2] << 63);
        res.limbs[2] = (m.limbs[2] >> 1) | (m.limbs[3] << 63);
        res.limbs[3] = m.limbs[3] >> 1;
        return res;
    }
    if constexpr (D == NORTH_EAST) { // << 17
        m = b & ~File15BB;
        res.limbs[0] = m.limbs[0] << 17;
        res.limbs[1] = (m.limbs[1] << 17) | (m.limbs[0] >> 47);
        res.limbs[2] = (m.limbs[2] << 17) | (m.limbs[1] >> 47);
        res.limbs[3] = (m.limbs[3] << 17) | (m.limbs[2] >> 47);
        return res;
    }
    if constexpr (D == NORTH_WEST) { // << 15
        m = b & ~File0BB;
        res.limbs[0] = m.limbs[0] << 15;
        res.limbs[1] = (m.limbs[1] << 15) | (m.limbs[0] >> 49);
        res.limbs[2] = (m.limbs[2] << 15) | (m.limbs[1] >> 49);
        res.limbs[3] = (m.limbs[3] << 15) | (m.limbs[2] >> 49);
        return res;
    }
    if constexpr (D == SOUTH_EAST) { // >> 15
        m = b & ~File15BB;
        res.limbs[0] = (m.limbs[0] >> 15) | (m.limbs[1] << 49);
        res.limbs[1] = (m.limbs[1] >> 15) | (m.limbs[2] << 49);
        res.limbs[2] = (m.limbs[2] >> 15) | (m.limbs[3] << 49);
        res.limbs[3] = m.limbs[3] >> 15;
        return res;
    }
    if constexpr (D == SOUTH_WEST) { // >> 17
        m = b & ~File0BB;
        res.limbs[0] = (m.limbs[0] >> 17) | (m.limbs[1] << 47);
        res.limbs[1] = (m.limbs[1] >> 17) | (m.limbs[2] << 47);
        res.limbs[2] = (m.limbs[2] >> 17) | (m.limbs[3] << 47);
        res.limbs[3] = m.limbs[3] >> 17;
        return res;
    }

    return res;
}

// ----------------------------------------
//  Attacks & Magic / PEXT Data Structures
// ----------------------------------------
struct PextEntry {
    uint64_t mask[4];
    uint32_t offset;
    uint8_t shift[3]; // Precalculated shift offsets
};

extern PextEntry kBishopDiagPext[SQUARE_NB];
extern PextEntry kBishopAntiDiagPext[SQUARE_NB];

extern const Bitboard* g_BishopDiagAttacksRaw;
extern const Bitboard* g_BishopAntiDiagAttacksRaw;

enum RayDirection { D_NE, D_NW, D_SE, D_SW, D_N, D_E, D_S, D_W };
extern Bitboard RayAttacks[SQUARE_NB][8];

// ------------------------------
//  Raycasting & PEXT Helpers
// ------------------------------

// Helper: 256-bit PEXT simulation using AVX2 64-bit PEXT (_pext_u64).
// Requires -mbmi2. This is used in the attack lookup hot-path if HasPext 
// is enabled. It iterates limbs but is faster than fallback raycasting.
inline uint64_t pext_256(const Bitboard& source, const PextEntry& entry) {
    uint64_t res = pext(source.limbs[0], entry.mask[0]);
    res |= pext(source.limbs[1], entry.mask[1]) << entry.shift[0];
    res |= pext(source.limbs[2], entry.mask[2]) << entry.shift[1];
    res |= pext(source.limbs[3], entry.mask[3]) << entry.shift[2];
    return res;
}

// Core Ray Casting Logic.
// This looks up the precomputed full ray, masks it with occupied squares,
// finds the first blocker, and masks off the part of the ray behind the blocker.
inline Bitboard get_ray_attack(int sq, RayDirection dir, const Bitboard& blockers) {
    Bitboard ray = RayAttacks[sq][dir];
    Bitboard b = ray & blockers;
    
    if (!b.is_zero()) {
        int blocker_idx;
        
        // Determine direction of index increase/decrease for LSB/MSB
        // North (+16), East (+1), NE (+17), NW (+15) -> Indices Increase -> LSB is first hit
        // South (-16), West (-1), SE (-15), SW (-17) -> Indices Decrease -> MSB is first hit
        if (dir == D_N || dir == D_E || dir == D_NE || dir == D_NW) 
            blocker_idx = lsb(b); // First blocker encountered going up
        else 
            blocker_idx = msb(b); // First blocker encountered going down (D_S, D_W, D_SE, D_SW)

        // Masking out the part of the ray starting from the blocker.
        // Stockfish standard: RayAttacks[sq] excludes sq.
        // So RayAttacks[blocker_idx] is the "tail" and excludes blocker_idx.
        // 'ray' (from sq) includes blocker_idx.
        // ray & ~tail removes the tail, keeping the segment from sq to blocker_idx (inclusive).
        return ray & ~RayAttacks[blocker_idx][dir];
    }
    return ray;
}

// ------------------------------
//  Attacks
// ------------------------------

// Rook attacks are computed instead of looked up, because lookup tables
// for rooks on this board take 16 MiB and mostly miss the caches. A rank
// lies within one 64-bit limb, and a file is gathered into one word with
// a bit per rank, so both lines are handled by line_attacks().

// The squares that a slider attacks on one line, given the line's
// occupancy and the masks of the line's squares below and above the
// slider. These are all in the same word, ordered along the line. This is
// the obstruction difference: everything from the highest blocker below
// (or bit 0) up to the lowest blocker above (or the top).
inline uint64_t line_attacks(uint64_t occupied, uint64_t lower, uint64_t upper) {
    const uint64_t below = occupied & lower;
    const uint64_t above = occupied & upper;
#if defined(__GNUC__) || defined(__clang__)
    const int highestBelow = 63 ^ __builtin_clzll(below | 1);
#else
    unsigned long highestBelow;
    _BitScanReverse64(&highestBelow, below | 1);
#endif
    return (lower | upper) & (2 * (above & (0 - above)) - (1ULL << highestBelow));
}

struct RookLineMasks {
    uint64_t rankLower, rankUpper;  // the rank's squares, as bits of the rook's limb
    uint64_t fileLower, fileUpper;  // the file's squares, as one bit per rank
};

extern RookLineMasks RookMasks[SQUARE_NB];

constexpr uint64_t LimbFileMask = 0x0001000100010001ULL;  // file 0 of a limb's four ranks

// The occupancy of file f, one bit per rank.
inline uint64_t file_to_ranks(const Bitboard& b, int f) {
    if constexpr (HasPext)
    {
        const uint64_t m = LimbFileMask << f;
        return pext(b.limbs[0], m) | pext(b.limbs[1], m) << 4 | pext(b.limbs[2], m) << 8
             | pext(b.limbs[3], m) << 12;
    }
    else
    {
        // Moves the bit of the limb's rank i to bit 48 + i. No other
        // product term reaches bits 48 to 51.
        auto ranks = [f](uint64_t limb) {
            return ((limb >> f) & LimbFileMask) * 0x0001000200040008ULL >> 48;
        };
        return ranks(b.limbs[0]) | ranks(b.limbs[1]) << 4 | ranks(b.limbs[2]) << 8
             | ranks(b.limbs[3]) << 12;
    }
}

// The inverse of file_to_ranks(): the squares of file f on the given ranks.
inline Bitboard ranks_to_file(uint64_t ranks, int f, uint64_t rankAttacks, int rankLimb) {
    uint64_t limb[4];
    if constexpr (HasPext)
    {
        const uint64_t m = LimbFileMask << f;
        for (int i = 0; i < 4; ++i)
            limb[i] = pdep(ranks >> 4 * i, m);
    }
    else
    {
        // Moves bit i of each nibble to bit 16 * i. The other product
        // terms fall outside the file.
        for (int i = 0; i < 4; ++i)
            limb[i] = ((ranks >> 4 * i & 0xF) * 0x0000200040008001ULL & LimbFileMask) << f;
    }
    // The rank's attacks are merged here so that the limbs are built once
    for (int i = 0; i < 4; ++i)
        limb[i] |= rankLimb == i ? rankAttacks : 0;
    return Bitboard(limb[0], limb[1], limb[2], limb[3]);
}

inline Bitboard get_rook_attacks(Square sq, const Bitboard& blockers) {
    const RookLineMasks& m = RookMasks[sq];
    const int            f = file_of(sq);
    const int            l = sq >> 6;
    return ranks_to_file(line_attacks(file_to_ranks(blockers, f), m.fileLower, m.fileUpper), f,
                         line_attacks(blockers.limbs[l], m.rankLower, m.rankUpper), l);
}

struct BishopLineMasks {
    uint64_t diagLower, diagUpper;
    uint64_t antiLower, antiUpper;
};

extern BishopLineMasks BishopMasks[SQUARE_NB];

inline uint64_t rotr(uint64_t x, int c) { return x >> (c & 63) | x << (-c & 63); }

template<int Step>
constexpr uint64_t DiagPattern = Step == 17 ? 0x0008000400020001ULL : 0x0000200040008001ULL;

template<int Step>
inline uint64_t diagonal_to_ranks(const Bitboard& b, int c) {
    constexpr uint64_t Gather = Step == 17 ? 0x0001000100010001ULL : 0x0001000400100040ULL;
    uint64_t ranks = 0;
    for (int i = 0; i < 4; ++i)
        ranks |= ((rotr(b.limbs[i], c + (Step - 16) * 4 * i) & DiagPattern<Step>) * Gather >> 48 & 0xF) << 4 * i;
    return ranks;
}

template<int Step>
inline uint64_t ranks_to_diagonal(uint64_t ranks, int c, int i) {
    constexpr uint64_t Spread = Step == 17 ? 0x0001000100010001ULL : 0x0000040010004001ULL;
    return rotr((ranks >> 4 * i & 0xF) * Spread & DiagPattern<Step>, -(c + (Step - 16) * 4 * i));
}

inline Bitboard get_bishop_attacks(Square sq, const Bitboard& blockers) {
    if constexpr (HasPext) {
        const PextEntry& d = kBishopDiagPext[sq];
        const PextEntry& a = kBishopAntiDiagPext[sq];
        return g_BishopDiagAttacksRaw[d.offset + pext_256(blockers, d)] |
               g_BishopAntiDiagAttacksRaw[a.offset + pext_256(blockers, a)];
    } else {
        const BishopLineMasks& m = BishopMasks[sq];
        const int c1 = file_of(sq) - rank_of(sq);
        const int c2 = file_of(sq) + rank_of(sq);
        const uint64_t diag = line_attacks(diagonal_to_ranks<17>(blockers, c1), m.diagLower, m.diagUpper);
        const uint64_t anti = line_attacks(diagonal_to_ranks<15>(blockers, c2), m.antiLower, m.antiUpper);
        uint64_t limb[4];
        for (int i = 0; i < 4; ++i)
            limb[i] = ranks_to_diagonal<17>(diag, c1, i) | ranks_to_diagonal<15>(anti, c2, i);
        return Bitboard(limb[0], limb[1], limb[2], limb[3]);
    }
}

template<PieceType Pt>
inline Bitboard attacks_bb(Square s, const Bitboard& occupied) {
    assert((Pt != PAWN) && (is_ok(s)));

    switch (Pt)
    {
    case BISHOP : return get_bishop_attacks(s, occupied);
    case ROOK   : return get_rook_attacks(s, occupied);
    case QUEEN  : return get_bishop_attacks(s, occupied) | get_rook_attacks(s, occupied);
    default     : return PseudoAttacks[Pt][s];
    }
}

// Non-template wrapper
inline Bitboard attacks_bb(PieceType pt, Square s, const Bitboard& occupied) {
    assert((pt != PAWN) && (is_ok(s)));
    switch (pt)
    {
    case BISHOP : return get_bishop_attacks(s, occupied);
    case ROOK   : return get_rook_attacks(s, occupied);
    case QUEEN  : return get_bishop_attacks(s, occupied) | get_rook_attacks(s, occupied);
    default     : return PseudoAttacks[pt][s];
    }
}

// Pawn attacks are direction/color specific
template<Color C>
inline Bitboard pawn_attacks_bb(const Bitboard& b) {
    // 4PC: Pawn attacks depend on color orientation.
    // Red (0) moves North (+16). Captures NW (+15), NE (+17).
    // Blue (1) moves East (+1). Captures NE (+17), SE (-15).
    // Yellow (2) moves South (-16). Captures SE (-15), SW (-17).
    // Green (3) moves West (-1). Captures SW (-17), NW (+15).
    
    // Note: We use the safe 'shift' template which handles edge wrapping logic internally.
    
    if constexpr (C == RED) {
        return shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b);
    } 
    else if constexpr (C == BLUE) {
        return shift<NORTH_EAST>(b) | shift<SOUTH_EAST>(b);
    }
    else if constexpr (C == YELLOW) {
        return shift<SOUTH_EAST>(b) | shift<SOUTH_WEST>(b);
    }
    else if constexpr (C == GREEN) {
        return shift<SOUTH_WEST>(b) | shift<NORTH_WEST>(b);
    }
}

} // namespace FFA

#endif // BITBOARD_H_INCLUDED