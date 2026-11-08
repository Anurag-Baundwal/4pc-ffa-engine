/*
  Stockfish 4PC, a 4-Player Chess Engine derived from Stockfish
  Bitboard Implementation
*/

#include "bitboard.h"
#include <cstring>
#include <iostream>
#include <random>

#if (defined(__GNUC__) || defined(__clang__)) && defined(__BMI2__) && (defined(__i386__) || defined(__x86_64__))
#include <immintrin.h>
#endif

namespace FFA {

// Global Tables
Bitboard LegalSquaresBB;
uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];

// Masks for wrapping logic (File 0 and File 15)
// Pattern: 1 every 16 bits.
// F0:  0x0001000100010001 (bits 0, 16, 32...)
// F15: 0x8000800080008000 (bits 15, 31, 47...)
static const uint64_t F0_Pat  = 0x0001000100010001ULL;
static const uint64_t F15_Pat = 0x8000800080008000ULL;

Bitboard File0BB  = Bitboard(F0_Pat, F0_Pat, F0_Pat, F0_Pat);
Bitboard File15BB = Bitboard(F15_Pat, F15_Pat, F15_Pat, F15_Pat);

PextEntry kBishopDiagPext[SQUARE_NB];
PextEntry kBishopAntiDiagPext[SQUARE_NB];

const Bitboard* g_BishopDiagAttacksRaw = nullptr;
const Bitboard* g_BishopAntiDiagAttacksRaw = nullptr;

Bitboard RayAttacks[SQUARE_NB][8];
RookLineMasks RookMasks[SQUARE_NB];
BishopLineMasks BishopMasks[SQUARE_NB];
// --------------------------------------------

namespace {

    // These tables store the attack bitboards.
    // They are split (Half-pieces) to keep size manageable.
    std::vector<Bitboard> kBishopDiagAttacksTable;
    std::vector<Bitboard> kBishopAntiDiagAttacksTable;

    // Helper: 256-bit PDEP simulation using AVX2 64-bit PDEP
    // Used to generate the attack tables (maps index -> blockers)
    Bitboard pdep_256(uint64_t source, const Bitboard& mask) {
        Bitboard result = Bitboard::zero();
        int current_shift = 0;
        
        for (int i = 0; i < 4; ++i) {
            uint64_t m = mask.limbs[i];
            int cnt = __builtin_popcountll(m);
            
            // We take 'cnt' bits from source starting at current_shift
            uint64_t src_chunk = (source >> current_shift); 
            
            #if defined(__BMI2__) && (defined(__i386__) || defined(__x86_64__))
            result.limbs[i] = _pdep_u64(src_chunk, m);
            #else
            uint64_t deposited = 0;
            for (uint64_t bb = 1; m; bb <<= 1) {
                uint64_t lowest = m & -m;
                if (src_chunk & bb)
                    deposited |= lowest;
                m &= m - 1;
            }
            result.limbs[i] = deposited;
            #endif
            
            current_shift += cnt;
        }
        return result;
    }

    // Used for PEXT table generation
    Bitboard calculate_attacks_for_rays(int sq, const Bitboard& blockers, RayDirection d1, RayDirection d2) {
        return get_ray_attack(sq, d1, blockers) | get_ray_attack(sq, d2, blockers);
    }
    
    Bitboard get_attack_mask(int sq, RayDirection d1, RayDirection d2) {
        Bitboard mask = (RayAttacks[sq][d1] | RayAttacks[sq][d2]);
        Bitboard ray1 = RayAttacks[sq][d1];
        Bitboard ray2 = RayAttacks[sq][d2];
        
        auto mask_edge = [&](const Bitboard& r, RayDirection d) {
            if (!r.is_zero()) {
                if (d == D_N || d == D_E || d == D_NE || d == D_NW) 
                     mask &= ~square_bb(msb(r)); // Mask the FURTHEST square (end of ray)
                else 
                     mask &= ~square_bb(lsb(r)); // Mask the FURTHEST square (end of ray)
            }
        };
        mask_edge(ray1, d1);
        mask_edge(ray2, d2);
        return mask;
    }

    // PEXT Table Generation
    void GeneratePextTablesForSquare(int sq, RayDirection d1, RayDirection d2, PextEntry& pext_entry, std::vector<Bitboard>& global_attack_table, uint32_t& current_offset) {
        if (!(LegalSquaresBB & square_bb(Square(sq)))) {
            std::memset(&pext_entry, 0, sizeof(PextEntry));
            return;
        }
        
        Bitboard mask = get_attack_mask(sq, d1, d2);
        
        pext_entry.mask[0] = mask.limbs[0];
        pext_entry.mask[1] = mask.limbs[1];
        pext_entry.mask[2] = mask.limbs[2];
        pext_entry.mask[3] = mask.limbs[3];

        // Precalculate the cumulative popcounts for the shifts
        int c0 = __builtin_popcountll(mask.limbs[0]);
        int c1 = __builtin_popcountll(mask.limbs[1]);
        int c2 = __builtin_popcountll(mask.limbs[2]);
        pext_entry.shift[0] = c0;
        pext_entry.shift[1] = c0 + c1;
        pext_entry.shift[2] = c0 + c1 + c2;

        int bits = c0 + c1 + c2 + __builtin_popcountll(mask.limbs[3]);
        uint64_t num_configs = 1ULL << bits;
        
        pext_entry.offset = current_offset;
        uint32_t initial_size = global_attack_table.size();
        global_attack_table.resize(initial_size + num_configs);

        for (uint64_t i = 0; i < num_configs; ++i) {
            Bitboard blockers = pdep_256(i, mask);
            global_attack_table[initial_size + i] = calculate_attacks_for_rays(sq, blockers, d1, d2);
        }
        current_offset += (uint32_t)num_configs;
    }
} // anon namespace

// ------------------------------
//  Public API Implementation
// ------------------------------

std::string Bitboards::pretty(const Bitboard& b) {
    std::string s = "+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+\n";
    for (int r = 15; r >= 0; --r) {
        for (int f = 0; f < 16; ++f) {
            Square sq = make_square(File(f), Rank(r));
            if (b.test(sq)) s += "| X ";
            else if (!(LegalSquaresBB.test(sq))) s += "|   "; // Blank for dead zones
            else s += "| . ";
        }
        s += "| " + std::to_string(r) + "\n";
        s += "+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+\n";
    }
    return s;
}

void Bitboards::init() {
    LegalSquaresBB = Bitboard::zero();
    
    // 1. Init Legal Squares (14x14 cross on 16x16)
    // Active area: R[1..14], C[1..14].
    for(int r=1; r<=14; ++r) {
        for(int c=1; c<=14; ++c) {
            bool dead = false;
            if (r <= 3 && c <= 3) dead = true;   // SW Corner
            if (r >= 12 && c <= 3) dead = true;  // NW Corner
            if (r <= 3 && c >= 12) dead = true;  // SE Corner
            if (r >= 12 && c >= 12) dead = true; // NE Corner
            
            if (!dead) {
                LegalSquaresBB.set_bit(make_square(File(c), Rank(r)));
            }
        }
    }

    // 2. Init Distances
    for (int s1 = 0; s1 < SQUARE_NB; ++s1) {
        for (int s2 = 0; s2 < SQUARE_NB; ++s2) {
            // Chess distance: max(dx, dy)
            int r1 = rank_of(Square(s1)), c1 = file_of(Square(s1));
            int r2 = rank_of(Square(s2)), c2 = file_of(Square(s2));
            SquareDistance[s1][s2] = std::max(std::abs(r1 - r2), std::abs(c1 - c2));
        }
    }

    // 3. Init Rays (Needed for BOTH Pext gen AND Fallback)
    // D_N (+16), D_E (+1), D_S (-16), D_W (-1)
    // D_NE (+17), D_NW (+15), D_SE (-15), D_SW (-17)
    
    int dr[] = { 1,  1, -1, -1,  1,  0, -1,  0}; // NE, NW, SE, SW, N, E, S, W
    int dc[] = { 1, -1,  1, -1,  0,  1,  0, -1};
    
    for (int s = 0; s < SQUARE_NB; ++s) {
        // We only generate attack tables for legal squares.
        if (!LegalSquaresBB.test(s)) continue;
        
        int r = rank_of(Square(s));
        int c = file_of(Square(s));
        
        for (int d = 0; d < 8; ++d) {
            int tr = r + dr[d];
            int tc = c + dc[d];
            
            // Slide along the ray
            while (tr >= 0 && tr < 16 && tc >= 0 && tc < 16) {
                Square target = make_square(File(tc), Rank(tr));
                
                // If we hit a wall (illegal square), the ray stops.
                if (!LegalSquaresBB.test(target)) break; 
                
                RayAttacks[s][d] |= square_bb(target);
                
                tr += dr[d];
                tc += dc[d];
            }
        }
    }

    // 4. Init Pseudo Attacks (Knight, King, Pawn)
    for (int s = 0; s < SQUARE_NB; ++s) {
        if (!LegalSquaresBB.test(s)) continue;
        
        // Knight
        int kn_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int kn_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for(int i=0; i<8; ++i) {
            int tr = rank_of(Square(s)) + kn_dr[i];
            int tc = file_of(Square(s)) + kn_dc[i];
            if (tr>=0 && tr<16 && tc>=0 && tc<16) {
                Square t = make_square(File(tc), Rank(tr));
                if(LegalSquaresBB.test(t)) PseudoAttacks[KNIGHT][s] |= square_bb(t);
            }
        }

        // King
        for (int d = 0; d < 8; ++d) {
            Bitboard ray = RayAttacks[s][d];
            if (!ray.is_zero()) {
                // The first square in the ray is the king attack
                // NE/NW/N/E (Indices 0,1,4,5) are increasing indices -> LSB
                Square t = (d < 4 && d != 2 && d != 3) || d == 4 || d == 5 ? lsb(ray) : msb(ray);
                PseudoAttacks[KING][s] |= square_bb(t);
            }
        }
        
        // Rook line masks. A rank or file has no dead squares between its
        // legal ones, so a line is all of its legal squares.
        RookLineMasks& m = RookMasks[s];
        const int r = rank_of(Square(s)), f = file_of(Square(s));
        for (int i = 0; i < 16; ++i)
        {
            const Square onRank = make_square(File(i), Rank(r));
            const Square onFile = make_square(File(f), Rank(i));
            if (i != f && LegalSquaresBB.test(onRank))
                (i < f ? m.rankLower : m.rankUpper) |= 1ULL << (int(onRank) & 63);
            if (i != r && LegalSquaresBB.test(onFile))
                (i < r ? m.fileLower : m.fileUpper) |= 1ULL << i;
        }

        auto ray_ranks = [](Bitboard b) {
            uint64_t ranks = 0;
            while (b)
                ranks |= 1ULL << rank_of(pop_lsb(b));
            return ranks;
        };
        BishopMasks[s] = {ray_ranks(RayAttacks[s][D_SW]), ray_ranks(RayAttacks[s][D_NE]),
                          ray_ranks(RayAttacks[s][D_SE]), ray_ranks(RayAttacks[s][D_NW])};

        // Sliders
        PseudoAttacks[ROOK][s]   = RayAttacks[s][D_N]  | RayAttacks[s][D_S]  | RayAttacks[s][D_E]  | RayAttacks[s][D_W];
        PseudoAttacks[BISHOP][s] = RayAttacks[s][D_NE] | RayAttacks[s][D_NW] | RayAttacks[s][D_SE] | RayAttacks[s][D_SW];
        PseudoAttacks[QUEEN][s]  = PseudoAttacks[ROOK][s] | PseudoAttacks[BISHOP][s];

        // Pawn Attacks
        PawnAttacks[RED][s]    = pawn_attacks_bb<RED>(square_bb(Square(s))) & LegalSquaresBB;
        PawnAttacks[BLUE][s]   = pawn_attacks_bb<BLUE>(square_bb(Square(s))) & LegalSquaresBB;
        PawnAttacks[YELLOW][s] = pawn_attacks_bb<YELLOW>(square_bb(Square(s))) & LegalSquaresBB;
        PawnAttacks[GREEN][s]  = pawn_attacks_bb<GREEN>(square_bb(Square(s))) & LegalSquaresBB;
    }

    // 5. Init PEXT Tables (Only if enabled)
    if constexpr (HasPext) {
        uint32_t current_offset;
        
        auto gen_pext = [&](PextEntry entries[], std::vector<Bitboard>& table, RayDirection d1, RayDirection d2) {
            current_offset = 0;
            table.clear();
            table.reserve(150000);
            for(int s=0; s<SQUARE_NB; ++s) {
                GeneratePextTablesForSquare(s, d1, d2, entries[s], table, current_offset);
            }
        };

        gen_pext(kBishopDiagPext, kBishopDiagAttacksTable, D_NE, D_SW);
        g_BishopDiagAttacksRaw = kBishopDiagAttacksTable.data();

        gen_pext(kBishopAntiDiagPext, kBishopAntiDiagAttacksTable, D_NW, D_SE);
        g_BishopAntiDiagAttacksRaw = kBishopAntiDiagAttacksTable.data();
    }

    // 6. Init BetweenBB / LineBB
    for(int s1=0; s1<SQUARE_NB; ++s1) {
        if (!LegalSquaresBB.test(s1)) continue;
        for(int s2=0; s2<SQUARE_NB; ++s2) {
            if (!LegalSquaresBB.test(s2)) continue;
            if (s1 == s2) continue;
            
            for(int d=0; d<8; ++d) {
                if (RayAttacks[s1][d].test(s2)) {
                    // Logic mapping:
                    // 0:NE <-> 3:SW
                    // 1:NW <-> 2:SE
                    // 4:N  <-> 6:S
                    // 5:E  <-> 7:W
                    int opp_map[] = {3, 2, 1, 0, 6, 7, 4, 5};
                    int od = opp_map[d];
                    
                    LineBB[s1][s2] = RayAttacks[s1][d] | RayAttacks[s1][od] | square_bb(Square(s1));
                    BetweenBB[s1][s2] = RayAttacks[s1][d] & RayAttacks[s2][od];
                    break;
                }
            }
        }
    }
}

} // namespace FFA
