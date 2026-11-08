/*
  Stockfish 4PC, a 4-Player Chess Engine derived from Stockfish
  FastUint256 Header
*/

#ifndef MYPROJECT_FAST_UINT256_HPP // Choose a unique guard name
#define MYPROJECT_FAST_UINT256_HPP

// 1. Include Dependencies
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map> // Needed only for the constructor from hex if that code uses it.
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector> // Needed for something? Check includes. string/array maybe implicit?
    // (The provided code doesn't actually use std::map in constructor)
    // Let's remove <vector> and <map> unless specifically needed by constructor/etc.

// 2. Intrinsics / uint128_t Detection
// --- DETECT COMPILER FOR INTRINSICS ---
#if defined(_MSC_VER)
#include <intrin.h>
#define FASTUINT_HAS_MSVC_INTRINSICS 1
#elif defined(__GNUC__) || defined(__clang__)
#define FASTUINT_HAS_GCC_CLANG_INTRINSICS 1
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h> // Includes general x86 intrinsics, popcnt, etc.
#define FASTUINT_HAS_X86_INTRINSICS 1
#endif
#endif
// --- DETECT __uint128_t SUPPORT ---
#if defined(__GNUC__) || defined(__clang__)
#if defined(__SIZEOF_INT128__)
#define FASTUINT_HAS_UINT128 1
using uint128_t = __uint128_t;
#endif
#endif
// --- Defaults ---
#ifndef FASTUINT_HAS_MSVC_INTRINSICS
#define FASTUINT_HAS_MSVC_INTRINSICS 0
#endif
#ifndef FASTUINT_HAS_GCC_CLANG_INTRINSICS
#define FASTUINT_HAS_GCC_CLANG_INTRINSICS 0
#endif
#ifndef FASTUINT_HAS_UINT128
#define FASTUINT_HAS_UINT128 0
#endif

// (Optional) 3. Namespace
namespace my_math {

// 4. Paste the FastUint256 struct definition (Version 2 - Boost Comparison
// Test)
struct FastUint256 {
  // Limbs are stored little-endian: limbs[0] is least significant
  alignas(32) std::array<uint64_t, 4> limbs;

  // --- Constructors ---
  constexpr FastUint256() : limbs{} {} // Zero initialization

  constexpr FastUint256(uint64_t low) : limbs{low, 0, 0, 0} {}

  constexpr FastUint256(uint64_t l0, uint64_t l1, uint64_t l2, uint64_t l3)
      : limbs{l0, l1, l2, l3} {}

  // Construct from hex string (e.g., "0x...", "123...ABC")
  FastUint256(const std::string &hex_str) : limbs{} {
    std::string s = hex_str;
    if (s.length() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s = s.substr(2);
    }
    if (s.empty()) {
      return; // Zero initialize
    }
    // Allow up to 64 hex chars
    if (s.length() > 64) {
      // Accept longer strings but effectively truncate from the left
      s = s.substr(s.length() - 64);
      // Or throw: throw std::length_error("Hex string too long for 256 bits");
    }

    size_t num_chars = s.length();
    size_t limb_idx = 0;
    size_t str_idx = num_chars;

    while (str_idx > 0 && limb_idx < 4) {
      size_t start = (str_idx > 16) ? (str_idx - 16) : 0;
      size_t count = str_idx - start;
      std::string limb_str = s.substr(start, count);

      unsigned long long limb_val;
      size_t processed_chars;
      try {
        // Handle empty limb string case which can happen with short input hex
        if (limb_str.empty()) {
          limb_val = 0;
          processed_chars = 0; // Ensure we don't falsely claim success
        } else {
          limb_val = std::stoull(limb_str, &processed_chars, 16);
        }

        if (processed_chars != limb_str.length()) {
          // This check might fail if stoull parses "0" from "" - handled above
          if (!limb_str.empty()) {
            throw std::invalid_argument("Invalid hex character");
          }
        }
      } catch (const std::invalid_argument &e) {
        throw std::invalid_argument(
            "Invalid hex string (char): " + std::string(e.what()) + " in '" +
            limb_str + "' from '" + hex_str + "'");
      } catch (const std::out_of_range &e) {
        throw std::out_of_range(
            "Hex string limb out of range: " + std::string(e.what()) + " in '" +
            limb_str + "'");
      } catch (const std::exception &e) {
        throw std::runtime_error(
            "Unexpected error parsing hex string: " + std::string(e.what()) +
            " in '" + limb_str + "'");
      }

      limbs[limb_idx] = limb_val;
      limb_idx++;
      str_idx = start;
    }
    // No need to throw here, remaining limbs are zero initialized
    // if (str_idx > 0) {
    //   throw std::runtime_error("Hex string parsing logic error");
    // }
  }

  // --- Single Bit Operations (Optimized) ---
  // Checks if bit at 'index' is set. Avoids full 256-bit logic.
  constexpr bool test(int index) const {
      return (limbs[index >> 6] & (1ULL << (index & 63))) != 0;
  }

  // Sets bit at 'index'.
  constexpr void set_bit(int index) {
      limbs[index >> 6] |= (1ULL << (index & 63));
  }

  // Clears bit at 'index'. Faster than "b &= b-1" for wide integers.
  constexpr void clear_bit(int index) {
      limbs[index >> 6] &= ~(1ULL << (index & 63));
  }

  // --- Constants ---
  static constexpr FastUint256 max() {
    constexpr uint64_t max64 = std::numeric_limits<uint64_t>::max();
    return FastUint256(max64, max64, max64, max64);
  }
  static constexpr FastUint256 zero() { return FastUint256(); }

  // --- Bitwise Operators ---
#if defined(__AVX2__)
  FastUint256 operator&(const FastUint256 &other) const {
    FastUint256 result;
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(limbs.data()));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(other.limbs.data()));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(result.limbs.data()), _mm256_and_si256(a, b));
    return result;
  }
  FastUint256 operator|(const FastUint256 &other) const {
    FastUint256 result;
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(limbs.data()));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(other.limbs.data()));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(result.limbs.data()), _mm256_or_si256(a, b));
    return result;
  }
  FastUint256 operator^(const FastUint256 &other) const {
    FastUint256 result;
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(limbs.data()));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(other.limbs.data()));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(result.limbs.data()), _mm256_xor_si256(a, b));
    return result;
  }
  FastUint256 operator~() const {
    FastUint256 result;
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(limbs.data()));
    __m256i ones = _mm256_set1_epi32(-1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(result.limbs.data()), _mm256_xor_si256(a, ones));
    return result;
  }

  FastUint256 &operator&=(const FastUint256 &other) { *this = *this & other; return *this; }
  FastUint256 &operator|=(const FastUint256 &other) { *this = *this | other; return *this; }
  FastUint256 &operator^=(const FastUint256 &other) { *this = *this ^ other; return *this; }

  bool operator==(const FastUint256 &other) const {
    __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(limbs.data()));
    __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(other.limbs.data()));
    __m256i cmp = _mm256_cmpeq_epi64(a, b);
    return _mm256_movemask_epi8(cmp) == -1; // All 32 bytes match
  }
  bool operator!=(const FastUint256 &other) const { return !(*this == other); }

#else
  // --- Portable Fallbacks (Used for constexpr or non-AVX2) ---
  constexpr FastUint256 operator&(const FastUint256 &other) const {
    return FastUint256(limbs[0] & other.limbs[0], limbs[1] & other.limbs[1],
                       limbs[2] & other.limbs[2], limbs[3] & other.limbs[3]);
  }
  constexpr FastUint256 operator|(const FastUint256 &other) const {
    return FastUint256(limbs[0] | other.limbs[0], limbs[1] | other.limbs[1],
                       limbs[2] | other.limbs[2], limbs[3] | other.limbs[3]);
  }
  constexpr FastUint256 operator^(const FastUint256 &other) const {
    return FastUint256(limbs[0] ^ other.limbs[0], limbs[1] ^ other.limbs[1],
                       limbs[2] ^ other.limbs[2], limbs[3] ^ other.limbs[3]);
  }
  constexpr FastUint256 operator~() const {
    return FastUint256(~limbs[0], ~limbs[1], ~limbs[2], ~limbs[3]);
  }

  constexpr FastUint256 &operator&=(const FastUint256 &other) {
    limbs[0] &= other.limbs[0];
    limbs[1] &= other.limbs[1];
    limbs[2] &= other.limbs[2];
    limbs[3] &= other.limbs[3];
    return *this;
  }
  constexpr FastUint256 &operator|=(const FastUint256 &other) {
    limbs[0] |= other.limbs[0];
    limbs[1] |= other.limbs[1];
    limbs[2] |= other.limbs[2];
    limbs[3] |= other.limbs[3];
    return *this;
  }
  constexpr FastUint256 &operator^=(const FastUint256 &other) {
    limbs[0] ^= other.limbs[0];
    limbs[1] ^= other.limbs[1];
    limbs[2] ^= other.limbs[2];
    limbs[3] ^= other.limbs[3];
    return *this;
  }

  constexpr bool operator==(const FastUint256 &other) const {
    return limbs[3] == other.limbs[3] && limbs[2] == other.limbs[2] &&
           limbs[1] == other.limbs[1] && limbs[0] == other.limbs[0];
  }
  constexpr bool operator!=(const FastUint256 &other) const { return !(*this == other); }
#endif

  // --- Shift Operators ---
  FastUint256 operator<<(size_t shift) const {
    if (shift == 0) return *this;
    if (shift >= 256) return FastUint256(); // Result is zero
  
    FastUint256 result = {};
    const size_t limb_shift = shift / 64;
    const size_t bit_shift = shift % 64;
  
    if (bit_shift == 0) { // Optimize for whole limb shifts
      for (size_t i = 0; i < 4 - limb_shift; ++i) {
        result.limbs[i + limb_shift] = limbs[i];
      }
    } else {
  #if FASTUINT_HAS_UINT128 // Use 128-bit integers if available
      uint64_t carry = 0;
      for (size_t i = 0; i < 4; ++i) {
        size_t target_idx = i + limb_shift;
        if (target_idx >= 4) break; // Stop if shifting out of bounds
  
        uint128_t wide_val = ((uint128_t)limbs[i] << bit_shift) | carry;
        result.limbs[target_idx] = (uint64_t)wide_val; // Lower 64 bits
        carry = (uint64_t)(wide_val >> 64); // Upper 64 bits become next carry
      }
      // Any remaining 'carry' after the loop is an overflow beyond 256 bits and is discarded.
      // The block that was previously here (starting with "--- Corrected portable shift logic...")
      // has been redundant and overwrote the uint128_t result.
  
  #else // Portable version without __uint128_t
      size_t target_idx_portable = limb_shift;
      uint64_t current_carry_portable = 0;
      const size_t carry_shift_portable = 64 - bit_shift;
  
      for (size_t src_idx = 0; src_idx < 4 && target_idx_portable < 4; ++src_idx) {
        uint64_t low_part = limbs[src_idx] << bit_shift;
        uint64_t next_carry =
            (bit_shift == 0)
                ? 0
                : (limbs[src_idx] >>
                   carry_shift_portable); // Bits shifted out high become next carry
        result.limbs[target_idx_portable] =
            low_part |
            current_carry_portable; // Combine low part with carry from previous
        current_carry_portable = next_carry;
        target_idx_portable++; // Move to next target limb
      }
      // Place final carry if space remains
      if (target_idx_portable < 4 && current_carry_portable != 0) {
        result.limbs[target_idx_portable] = current_carry_portable;
      }
  #endif
    }
    return result;
  }

  FastUint256 operator>>(size_t shift) const {
    if (shift == 0) return *this;
    if (shift >= 256) return FastUint256(); // Result is zero

    FastUint256 result = {};
    const size_t limb_shift = shift / 64;
    const size_t bit_shift = shift % 64;

    if (bit_shift == 0) { // Optimize for whole limb shifts
      for (size_t i = limb_shift; i < 4; ++i) {
        result.limbs[i - limb_shift] = limbs[i];
      }
    } else {
#if FASTUINT_HAS_UINT128 // Use 128-bit integers if available
      const size_t carry_shift = 64 - bit_shift;
      // Iterate from most significant limb downwards
      for (int i = 3; i >= (int)limb_shift; --i) {
        int target_limb = i - (int)limb_shift;

        // Get carry from the limb 'above' (more significant) if it exists
        uint64_t carry_in = (i < 3) ? limbs[i + 1] : 0;

        // Combine bits shifted down from current limb with carry bits shifted
        // in
        uint128_t wide_val =
            (uint128_t)limbs[i] >> bit_shift; // Shift current limb down
        wide_val |= (uint128_t)carry_in
                    << carry_shift; // Bring in carry bits from the left

        result.limbs[target_limb] = (uint64_t)wide_val;
        // No need to explicitly track carry variable here, as we process
        // MSB->LSB
      }
#else // Portable version without __uint128_t
      // Portable version
      const size_t carry_shift = 64 - bit_shift;
      // Iterate destination limbs from Most Significant to Least Significant
      for (int dest_idx = 3 - (int)limb_shift; dest_idx >= 0; --dest_idx) {
        size_t src_idx_low =
            dest_idx +
            limb_shift; // Limb providing the lower bits (after shift)

        uint64_t low_part =
            limbs[src_idx_low] >> bit_shift; // Current limb shifted right
        uint64_t high_part =
            0; // Bits coming from the limb above (more significant)

        if (src_idx_low < 3) { // Check if there is a limb above
          size_t src_idx_high = src_idx_low + 1;
          // Get the high bits from the limb above that shift into the current
          // position
          high_part = limbs[src_idx_high] << carry_shift;
        }
        result.limbs[dest_idx] = low_part | high_part; // Combine parts
      }
#endif
    }
    return result;
  }

  FastUint256 &operator<<=(size_t shift) {
    *this = *this << shift;
    return *this;
  }
  FastUint256 &operator>>=(size_t shift) {
    *this = *this >> shift;
    return *this;
  }

  // --- Comparison Operators ---
  constexpr bool operator<(const FastUint256 &other) const {
    // Compare from most significant limb
    for (int i = 3; i >= 0; --i) {
      if (limbs[i] < other.limbs[i]) return true;
      if (limbs[i] > other.limbs[i]) return false;
    }
    return false; // Equal
  }
  constexpr bool operator>(const FastUint256 &other) const {
    return other < *this;
  }
  constexpr bool operator<=(const FastUint256 &other) const {
    return !(other < *this);
  }
  constexpr bool operator>=(const FastUint256 &other) const {
    return !(*this < other);
  }

  // Explicit bool conversion checks if non-zero
  constexpr explicit operator bool() const {
#if defined(__AVX2__)
    return limbs[0] != 0 || limbs[1] != 0 || limbs[2] != 0 || limbs[3] != 0;
#else
    return (limbs[0] | limbs[1] | limbs[2] | limbs[3]) != 0;
#endif
  }

  // --- Arithmetic Operators ---
  FastUint256 operator+(const FastUint256 &other) const {
    FastUint256 result = {};
#if FASTUINT_HAS_MSVC_INTRINSICS && defined(_M_X64) // Intrinsics only available on x64
    unsigned char carry = 0;
    carry = _addcarry_u64(carry, limbs[0], other.limbs[0], &result.limbs[0]);
    carry = _addcarry_u64(carry, limbs[1], other.limbs[1], &result.limbs[1]);
    carry = _addcarry_u64(carry, limbs[2], other.limbs[2], &result.limbs[2]);
    _addcarry_u64(carry, limbs[3], other.limbs[3], &result.limbs[3]);
    // Overflow carry is discarded in standard 256-bit addition
#elif FASTUINT_HAS_GCC_CLANG_INTRINSICS && \
    defined(__x86_64__) // Intrinsics typically require x86_64

#if defined(__ADX__)
    unsigned char c = 0;
    c = _addcarry_u64(c, limbs[0], other.limbs[0], (unsigned long long*)&result.limbs[0]);
    c = _addcarry_u64(c, limbs[1], other.limbs[1], (unsigned long long*)&result.limbs[1]);
    c = _addcarry_u64(c, limbs[2], other.limbs[2], (unsigned long long*)&result.limbs[2]);
    _addcarry_u64(c, limbs[3], other.limbs[3], (unsigned long long*)&result.limbs[3]);
#elif FASTUINT_HAS_UINT128       // Fallback to uint128 if intrinsics aren't ideal
    uint128_t sum = 0;
    sum = (uint128_t)limbs[0] + other.limbs[0];
    result.limbs[0] = (uint64_t)sum;
    sum = (uint128_t)limbs[1] + other.limbs[1] + (uint64_t)(sum >> 64);
    result.limbs[1] = (uint64_t)sum;
    sum = (uint128_t)limbs[2] + other.limbs[2] + (uint64_t)(sum >> 64);
    result.limbs[2] = (uint64_t)sum;
    sum = (uint128_t)limbs[3] + other.limbs[3] + (uint64_t)(sum >> 64);
    result.limbs[3] = (uint64_t)sum;
#else                   // Portable fallback
    uint64_t carry_p = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t sum_nocarry = a + b;
      uint64_t res_limb = sum_nocarry + carry_p;
      result.limbs[i] = res_limb;
      carry_p =
          (res_limb < sum_nocarry) |
          (sum_nocarry < a); // Carry if result wrapped or initial sum wrapped
    }
#endif                  // End __ADX__ / HAS_UINT128 / Portable block
#elif FASTUINT_HAS_UINT128       // Primary non-intrinsic path if uint128 available
    uint128_t sum = 0;
    sum = (uint128_t)limbs[0] + other.limbs[0];
    result.limbs[0] = (uint64_t)sum;
    sum = (uint128_t)limbs[1] + other.limbs[1] + (uint64_t)(sum >> 64);
    result.limbs[1] = (uint64_t)sum;
    sum = (uint128_t)limbs[2] + other.limbs[2] + (uint64_t)(sum >> 64);
    result.limbs[2] = (uint64_t)sum;
    sum = (uint128_t)limbs[3] + other.limbs[3] + (uint64_t)(sum >> 64);
    result.limbs[3] = (uint64_t)sum;
#else                   // Portable fallback if no intrinsics and no uint128
    // Portable fallback
    uint64_t carry = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t sum_nocarry = a + b;
      uint64_t res_limb = sum_nocarry + carry;
      result.limbs[i] = res_limb;
      // Carry detect: if adding carry caused overflow OR adding a+b caused
      // overflow
      carry = (res_limb < sum_nocarry) | (sum_nocarry < a);
    }
#endif
    return result;
  }

  FastUint256 operator-(const FastUint256 &other) const {
    FastUint256 result = {};
#if FASTUINT_HAS_MSVC_INTRINSICS && defined(_M_X64)
    unsigned char borrow = 0;
    borrow = _subborrow_u64(borrow, limbs[0], other.limbs[0], &result.limbs[0]);
    borrow = _subborrow_u64(borrow, limbs[1], other.limbs[1], &result.limbs[1]);
    borrow = _subborrow_u64(borrow, limbs[2], other.limbs[2], &result.limbs[2]);
    _subborrow_u64(borrow, limbs[3], other.limbs[3], &result.limbs[3]);
    // Underflow borrow is discarded
#elif FASTUINT_HAS_GCC_CLANG_INTRINSICS && \
    defined(__x86_64__) // Intrinsics typically require x86_64

#if defined(__ADX__)
    unsigned char b = 0;
    b = _subborrow_u64(b, limbs[0], other.limbs[0], (unsigned long long*)&result.limbs[0]);
    b = _subborrow_u64(b, limbs[1], other.limbs[1], (unsigned long long*)&result.limbs[1]);
    b = _subborrow_u64(b, limbs[2], other.limbs[2], (unsigned long long*)&result.limbs[2]);
    _subborrow_u64(b, limbs[3], other.limbs[3], (unsigned long long*)&result.limbs[3]);
#elif FASTUINT_HAS_UINT128    // Fallback to uint128 (less direct for subtraction) or
                     // portable
    // Portable fallback simulation is better here than forcing uint128
    uint64_t borrow_p = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t diff_noborrow = a - b;
      uint64_t res_limb = diff_noborrow - borrow_p;
      result.limbs[i] = res_limb;
      borrow_p = (res_limb > diff_noborrow) |
                 (diff_noborrow >
                  a); // Borrow if result wrapped or initial diff wrapped
    }
#else                // Portable fallback
    uint64_t borrow_p = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t diff_noborrow = a - b;
      uint64_t res_limb = diff_noborrow - borrow_p;
      result.limbs[i] = res_limb;
      borrow_p = (res_limb > diff_noborrow) | (diff_noborrow > a);
    }
#endif               // End __ADX__ / HAS_UINT128 / Portable block
#elif FASTUINT_HAS_UINT128    // Primary non-intrinsic path if uint128 available (less
                     // direct for subtraction)
    // Portable logic is generally cleaner for subtraction without intrinsics
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t diff_noborrow = a - b;
      uint64_t res_limb = diff_noborrow - borrow;
      result.limbs[i] = res_limb;
      borrow = (res_limb > diff_noborrow) | (diff_noborrow > a);
    }
#else                // Portable fallback if no intrinsics and no uint128
    // Portable fallback
    uint64_t borrow = 0;
    for (int i = 0; i < 4; ++i) {
      uint64_t a = limbs[i];
      uint64_t b = other.limbs[i];
      uint64_t diff_noborrow = a - b;
      uint64_t res_limb = diff_noborrow - borrow;
      result.limbs[i] = res_limb;
      borrow = (res_limb > diff_noborrow) | (diff_noborrow > a);
    }
#endif
    return result;
  }

  // Multiplication by a 64-bit scalar
  FastUint256 operator*(uint64_t scalar) const {
    FastUint256 result = {};
#if FASTUINT_HAS_UINT128
    uint128_t partial_prod = 0;
    uint64_t carry = 0;

    // Multiply limb 0
    partial_prod = (uint128_t)limbs[0] * scalar;
    result.limbs[0] = (uint64_t)partial_prod;
    carry =
        (uint64_t)(partial_prod >> 64); // Carry out from limb 0 multiplication

    // Multiply limb 1 and add carry from limb 0
    partial_prod = (uint128_t)limbs[1] * scalar + carry;
    result.limbs[1] = (uint64_t)partial_prod;
    carry = (uint64_t)(partial_prod >> 64); // Carry out from limb 1

    // Multiply limb 2 and add carry from limb 1
    partial_prod = (uint128_t)limbs[2] * scalar + carry;
    result.limbs[2] = (uint64_t)partial_prod;
    carry = (uint64_t)(partial_prod >> 64); // Carry out from limb 2

    // Multiply limb 3 and add carry from limb 2
    partial_prod = (uint128_t)limbs[3] * scalar + carry;
    result.limbs[3] = (uint64_t)partial_prod;
    // Carry out from limb 3 is discarded (overflow)
#else
    // Portable fallback simulation: Standard grade-school multiplication
    uint64_t carry = 0; // Initialize carry for the first limb multiplication
    for (int i = 0; i < 4; ++i) {
      // Simulate 64x64->128 multiplication portably
      uint64_t u = limbs[i];
      uint64_t v = scalar;
      uint64_t u0 = u & 0xFFFFFFFF, u1 = u >> 32;
      uint64_t v0 = v & 0xFFFFFFFF, v1 = v >> 32;

      uint64_t t = u0 * v0;         // u0 * v0
      uint64_t w0 = t & 0xFFFFFFFF; // Lower 32 bits of t
      uint64_t k = t >> 32;         // Upper 32 bits of t

      t = u1 * v0 + k;              // u1 * v0 + carry
      uint64_t w1 = t & 0xFFFFFFFF; // Lower 32 bits
      uint64_t w2 = t >> 32;        // Upper 32 bits

      t = u0 * v1 + w1; // u0 * v1 + w1 (lower half of middle term)
      k = t >> 32;      // Carry from middle term addition

      // Low 64 bits of u*v product
      uint64_t product_low = (t << 32) + w0;
      // High 64 bits of u*v product
      uint64_t product_high = u1 * v1 + w2 + k;

      // Now add the carry from the previous limb's operation
      uint64_t temp_sum =
          result.limbs[i] +
          carry; // Add previous carry to current result (initially 0)
      uint64_t carry1 = (temp_sum < carry); // Did adding carry overflow?

      temp_sum += product_low; // Add low part of product
      uint64_t carry2 =
          (temp_sum < product_low); // Did adding product_low overflow?

      result.limbs[i] = temp_sum; // Final result for this limb
      carry =
          product_high + carry1 + carry2; // Next carry is high part + overflows
    }
    // Final carry is discarded
#endif
    return result;
  }

  // Full 256-bit by 256-bit multiplication
  FastUint256 operator*(const FastUint256 &other) const {
    FastUint256 result = {};

  #if FASTUINT_HAS_UINT128
    // Implements Schoolbook multiplication algorithm (optimized for 256-bit result):
    // For each result limb R[i] (where i = 0 to 3):
    // 1. Start with carry C_in from calculation of R[i-1]. Let Sum = C_in.
    // 2. Initialize HighPartsSum = 0.
    // 3. For each product p_jk = limbs[j] * other.limbs[k] where j+k=i:
    //    a. Add low part to sum: Sum += lo(p_jk)
    //    b. Add high part to high-parts accumulator: HighPartsSum += hi(p_jk)
    // 4. R[i] = lo(Sum)
    // 5. Carry-out C_out for next stage = hi(Sum) + HighPartsSum.

    uint128_t current_limb_sum = 0; // Holds Sum (C_in + sum(lo(p_jk))) for the current result limb calculation.
                                    // After calculating R[i], it's updated to hold C_out(i) (the Carry-in for R[i+1]).
    uint128_t product_high_parts_sum = 0; // Holds HighPartsSum (sum(hi(p_jk))) for the products contributing to the current R[i]. Reset for each i.

    // --- Calculate result.limbs[0] (i=0) ---
    // Carry-in C_in(-1) is 0. Only product is p00.
    // General Pattern applied to i=0:
    //   current_limb_sum = 0; // C_in(-1)
    //   product_high_parts_sum = 0;
    //   uint128_t p00 = (uint128_t)limbs[0] * other.limbs[0];
    //   current_limb_sum += (uint64_t)p00;       // Sum = 0 + lo(p00)
    //   product_high_parts_sum += (p00 >> 64); // HighPartsSum = 0 + hi(p00)
    //   result.limbs[0] = (uint64_t)current_limb_sum; // R[0] = lo(p00)
    //   current_limb_sum = (current_limb_sum >> 64) + product_high_parts_sum; // C_out(0) = (lo(p00)>>64) + hi(p00) = 0 + hi(p00)
    // Shortcut: Directly calculate R[0] and C_out(0) for the i=0 case.
    uint128_t p00 = (uint128_t)limbs[0] * other.limbs[0];
    result.limbs[0] = (uint64_t)p00;          // R[0] = lo(p00)
    current_limb_sum = (p00 >> 64);           // Initialize current_limb_sum with C_out(0) = hi(p00) (Carry-in for R[1])


    // --- Calculate result.limbs[1] (i=1) ---
    // Carry-in C_in(0) is already in current_limb_sum.
    // Products: p01, p10
    product_high_parts_sum = 0; // Reset high parts sum for this stage

    uint128_t p01 = (uint128_t)limbs[0] * other.limbs[1];
    uint128_t p10 = (uint128_t)limbs[1] * other.limbs[0];

    // Accumulate Sum_1 = C_in(0) + lo(p01) + lo(p10)
    current_limb_sum += (uint64_t)p01;
    current_limb_sum += (uint64_t)p10;

    // Accumulate product_high_parts_sum = hi(p01) + hi(p10)
    product_high_parts_sum = (p01 >> 64);  // First term, equivalent to 0 + hi(p01) since product_high_parts_sum was 0
    product_high_parts_sum += (p10 >> 64); // Add subsequent terms

    // Assign result limb and calculate Carry-out C_out(1) (Carry-in for R[2])
    result.limbs[1] = (uint64_t)current_limb_sum; // R[1] = lo(Sum_1)
    current_limb_sum = (current_limb_sum >> 64) + product_high_parts_sum; // C_out(1) = hi(Sum_1) + product_high_parts_sum


    // --- Calculate result.limbs[2] (i=2) ---
    // Carry-in C_in(1) is in current_limb_sum.
    // Products: p02, p11, p20
    product_high_parts_sum = 0; // Reset high parts sum for this stage

    uint128_t p02 = (uint128_t)limbs[0] * other.limbs[2];
    uint128_t p11 = (uint128_t)limbs[1] * other.limbs[1];
    uint128_t p20 = (uint128_t)limbs[2] * other.limbs[0];

    // Accumulate Sum_2 = C_in(1) + lo(p02) + lo(p11) + lo(p20)
    current_limb_sum += (uint64_t)p02;
    current_limb_sum += (uint64_t)p11;
    current_limb_sum += (uint64_t)p20;

    // Accumulate product_high_parts_sum = hi(p02) + hi(p11) + hi(p20)
    product_high_parts_sum = (p02 >> 64);  // First term
    product_high_parts_sum += (p11 >> 64); // Subsequent terms
    product_high_parts_sum += (p20 >> 64);

    // Assign result limb and calculate Carry-out C_out(2) (Carry-in for R[3])
    result.limbs[2] = (uint64_t)current_limb_sum; // R[2] = lo(Sum_2)
    current_limb_sum = (current_limb_sum >> 64) + product_high_parts_sum; // C_out(2) = hi(Sum_2) + product_high_parts_sum


    // --- Calculate result.limbs[3] (i=3) ---
    // Carry-in C_in(2) is in current_limb_sum.
    // Products: p03, p12, p21, p30
    // We only need R[3], not the final carry C_out(3), so calculating product_high_parts_sum is unnecessary here.
    // product_high_parts_sum = 0; // Not strictly needed

    uint128_t p03 = (uint128_t)limbs[0] * other.limbs[3];
    uint128_t p12 = (uint128_t)limbs[1] * other.limbs[2];
    uint128_t p21 = (uint128_t)limbs[2] * other.limbs[1];
    uint128_t p30 = (uint128_t)limbs[3] * other.limbs[0];

    // Accumulate Sum_3 = C_in(2) + lo(p03) + lo(p12) + lo(p21) + lo(p30)
    current_limb_sum += (uint64_t)p03;
    current_limb_sum += (uint64_t)p12;
    current_limb_sum += (uint64_t)p21;
    current_limb_sum += (uint64_t)p30;

    // Assign final result limb. The final carry C_out(3) = (current_limb_sum >> 64) + sum(hi(p_jk)) is discarded.
    result.limbs[3] = (uint64_t)current_limb_sum; // R[3] = lo(Sum_3)

  #else // Portable fallback

    // Uses existing FastUint256 * uint64_t, operator<<, and operator+
    // Conceptually calculates: result = (A * b0) + ((A * b1) << 64) + ((A * b2) << 128) + ((A * b3) << 192)
    // where A is *this and B is other (b0..b3 are limbs of other).
    FastUint256 term0 = (*this) * other.limbs[0];
    FastUint256 term1 = ((*this) * other.limbs[1]) << 64;
    FastUint256 term2 = ((*this) * other.limbs[2]) << 128;
    FastUint256 term3 = ((*this) * other.limbs[3]) << 192;

    result = term0 + term1 + term2 + term3;

    // WARNING: This portable fallback is simpler to write but likely less
    // efficient than a direct portable implementation of the schoolbook
    // algorithm using portable 64x64->128 multiplies (like the one used
    // in operator*(uint64_t)). This version involves multiple full 256-bit
    // additions and shifts. The __uint128_t version is strongly preferred
    // for performance when available.

  #endif // FASTUINT_HAS_UINT128
    return result;
  }

  FastUint256 &operator+=(const FastUint256 &other) {
    *this = *this + other;
    return *this;
  }
  FastUint256 &operator-=(const FastUint256 &other) {
    *this = *this - other;
    return *this;
  }
  FastUint256 &operator*=(uint64_t scalar) {
    *this = *this * scalar;
    return *this;
  }
  FastUint256 &operator*=(const FastUint256 &other) {
    *this = *this * other;
    return *this;
  }

  // --- Utility Functions ---
  constexpr bool is_zero() const {
#if defined(__AVX2__)
    return limbs[0] == 0 && limbs[1] == 0 && limbs[2] == 0 && limbs[3] == 0;
#else
    return (limbs[0] | limbs[1] | limbs[2] | limbs[3]) == 0;
#endif
  }

  // Popcount (number of set bits)
  int popcount() const {
    int count = 0;
#if FASTUINT_HAS_GCC_CLANG_INTRINSICS
    count += __builtin_popcountll(limbs[0]);
    count += __builtin_popcountll(limbs[1]);
    count += __builtin_popcountll(limbs[2]);
    count += __builtin_popcountll(limbs[3]);
#elif FASTUINT_HAS_MSVC_INTRINSICS && defined(_M_X64) &&                                \
    defined(__AVX__) // MSVC popcnt intrinsic often tied to AVX? Check docs. Or
                    // just use __popcnt64 if defined.
    // Assume __popcnt64 is available if MSVC intrinsics are generally enabled
    // for x64
    count += (int)__popcnt64(limbs[0]);
    count += (int)__popcnt64(limbs[1]);
    count += (int)__popcnt64(limbs[2]);
    count += (int)__popcnt64(limbs[3]);
#else
    // Portable fallback (Kernighan's way)
    for (uint64_t limb : limbs) {
      uint64_t l = limb; // Operate on a copy
      while (l != 0) {
        l &= (l - 1); // Clear the least significant bit set
        count++;
      }
    }
#endif
    return count;
  }

  // Count leading zeros (from MSB)
  int clz() const {
#if FASTUINT_HAS_GCC_CLANG_INTRINSICS
    if (limbs[3]) return __builtin_clzll(limbs[3]);
    if (limbs[2]) return 64 + __builtin_clzll(limbs[2]);
    if (limbs[1]) return 128 + __builtin_clzll(limbs[1]);
    if (limbs[0]) return 192 + __builtin_clzll(limbs[0]);
#elif FASTUINT_HAS_MSVC_INTRINSICS && defined(_M_X64) // Use _BitScanReverse64
    unsigned long index;
    if (_BitScanReverse64(&index, limbs[3])) return (int)(63 - index);
    if (_BitScanReverse64(&index, limbs[2])) return 64 + (int)(63 - index);
    if (_BitScanReverse64(&index, limbs[1])) return 128 + (int)(63 - index);
    if (_BitScanReverse64(&index, limbs[0])) return 192 + (int)(63 - index);
#else
    // Portable fallback
    for (int i = 3; i >= 0; --i) {
      if (limbs[i] != 0) {
        uint64_t limb = limbs[i];
        int limb_clz = 0;
        // Find clz for 64-bit limb manually (simple iterative or binary search)
        if (limb == 0)
          limb_clz = 64; // Should not happen based on outer check
        else {
          // Efficient portable CLZ for uint64_t
          if ((limb >> 32) == 0) {
            limb_clz += 32;
            limb <<= 32;
          }
          if ((limb >> 48) == 0) {
            limb_clz += 16;
            limb <<= 16;
          }
          if ((limb >> 56) == 0) {
            limb_clz += 8;
            limb <<= 8;
          }
          if ((limb >> 60) == 0) {
            limb_clz += 4;
            limb <<= 4;
          }
          if ((limb >> 62) == 0) {
            limb_clz += 2;
            limb <<= 2;
          }
          if ((limb >> 63) == 0) {
            limb_clz += 1;
          }
        }
        return (3 - i) * 64 + limb_clz;
      }
    }
#endif
    return 256; // Value is zero
  }

  // Count trailing zeros (from LSB)
  int ctz() const {
#if FASTUINT_HAS_GCC_CLANG_INTRINSICS
    if (limbs[0]) return __builtin_ctzll(limbs[0]);
    if (limbs[1]) return 64 + __builtin_ctzll(limbs[1]);
    if (limbs[2]) return 128 + __builtin_ctzll(limbs[2]);
    if (limbs[3]) return 192 + __builtin_ctzll(limbs[3]);
#elif FASTUINT_HAS_MSVC_INTRINSICS && defined(_M_X64) // Use _BitScanForward64
    unsigned long index;
    if (_BitScanForward64(&index, limbs[0])) return (int)index;
    if (_BitScanForward64(&index, limbs[1])) return 64 + (int)index;
    if (_BitScanForward64(&index, limbs[2])) return 128 + (int)index;
    if (_BitScanForward64(&index, limbs[3])) return 192 + (int)index;
#else
    // Portable fallback
    for (int i = 0; i < 4; ++i) {
      if (limbs[i] != 0) {
        uint64_t limb = limbs[i];
        int limb_ctz = 0;
        // Find ctz for 64-bit limb manually (simple iterative or binary search)
        if (limb == 0)
          limb_ctz = 64; // Should not happen based on outer check
        else {
          // Efficient portable CTZ for uint64_t (De Bruijn sequence method is
          // faster but complex) Simpler iterative method:
          uint64_t mask = 1ULL;
          while ((limb & mask) == 0) {
            limb_ctz++;
            mask <<= 1;
            if (mask == 0) {
              limb_ctz = 64;
              break;
            } // Should not happen if limb != 0
          }
        }
        return i * 64 + limb_ctz;
      }
    }
#endif
    return 256; // Value is zero
  }

  // --- Conversion/Output ---
  std::string to_hex_string(bool include_prefix = true) const {
    std::stringstream ss;
    if (include_prefix) {
      ss << "0x";
    }
    bool leading_zeros = true;
    if (is_zero()) {
      ss << "0";
      return ss.str();
    }

    for (int i = 3; i >= 0; --i) {
      if (limbs[i] == 0 && leading_zeros && i > 0) {
        continue; // Skip leading zero limbs entirely
      }

      if (leading_zeros) {
        // Print the first non-zero limb without padding
        ss << std::hex << limbs[i];
        leading_zeros = false;
      } else {
        // Print subsequent limbs with full 16-char padding
        ss << std::hex << std::setfill('0') << std::setw(16) << limbs[i];
      }
    }
    return ss.str();
  }

  // Explicit conversion to uint64_t (returns least significant limb)
  explicit operator uint64_t() const { return limbs[0]; }

}; // end struct FastUint256

// 5. Add Free Functions
inline FastUint256 operator*(uint64_t scalar, const FastUint256 &val) {
  return val * scalar;
}

inline std::ostream &operator<<(std::ostream &os, const FastUint256 &val) {
  os << val.to_hex_string();
  return os;
}

} // namespace my_math

// 6. Close Include Guard
#endif // MYPROJECT_FAST_UINT256_HPP
