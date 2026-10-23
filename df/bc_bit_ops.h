#pragma once

// bc_bit_ops.h
//
// Portable intrinsic wrappers for the bit operations used by df::bitset<N>
// and df::bit_array. Provides df::ctz64 (count trailing zeros) and
// df::popcount64 (count set bits) without requiring C++20's <bit> header.
//
// ctz64(x) requires x != 0. Returns 0..63.
// popcount64(x) returns 0..64.
//
// Style: snake_case, One True Brace Style.

#include <stdint.h>
#include <assert.h>

#if defined(_MSC_VER) && defined(_M_X64)
#   include <intrin.h>
#   pragma intrinsic(_BitScanForward64)
#   pragma intrinsic(__popcnt64)
#endif

namespace df {

inline unsigned ctz64(uint64_t x) noexcept {
    assert(x != 0);
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long index = 0;
    _BitScanForward64(&index, x);
    return static_cast<unsigned>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(x));
#else
    unsigned n = 0;
    while ((x & 1) == 0) {
        x >>= 1;
        ++n;
    }
    return n;
#endif
}

inline unsigned popcount64(uint64_t x) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    return static_cast<unsigned>(__popcnt64(x));
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_popcountll(x));
#else
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<unsigned>((x * 0x0101010101010101ull) >> 56);
#endif
}

} // namespace df