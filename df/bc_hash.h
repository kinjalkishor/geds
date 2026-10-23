#pragma once

// bc_hash.h
//
// Single-header wyhash port. C stdlib headers only; no C++ STL.
//
// Port of https://github.com/wangyi-fudan/wyhash
// WYHASH_CONDOM = 1, WYHASH_32BIT_MUM = 0.
//
// Style: snake_case, One True Brace Style.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER) && defined(_M_X64)
#   include <intrin.h>
#   pragma intrinsic(_umul128)
#endif

namespace df { namespace wy {

namespace detail {

inline void wymum(uint64_t* A, uint64_t* B) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = *A;
    r *= *B;
    *A ^= (uint64_t)r;
    *B ^= (uint64_t)(r >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    uint64_t hi = 0;
    uint64_t lo = _umul128(*A, *B, &hi);
    *A ^= lo;
    *B ^= hi;
#else
    uint64_t ha = *A >> 32, hb = *B >> 32;
    uint64_t la = (uint32_t)*A, lb = (uint32_t)*B;
    uint64_t rh = ha * hb;
    uint64_t rm0 = ha * lb;
    uint64_t rm1 = hb * la;
    uint64_t rl = la * lb;
    uint64_t t = rl + (rm0 << 32);
    uint64_t c = (t < rl);
    uint64_t lo = t + (rm1 << 32);
    c += (lo < t);
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    *A ^= lo;
    *B ^= hi;
#endif
}

inline uint64_t wymix(uint64_t A, uint64_t B) {
    wymum(&A, &B);
    return A ^ B;
}

#ifndef WYHASH_LITTLE_ENDIAN
#   if defined(_WIN32) || defined(__LITTLE_ENDIAN__) \
       || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#       define WYHASH_LITTLE_ENDIAN 1
#   elif defined(__BIG_ENDIAN__) \
       || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#       define WYHASH_LITTLE_ENDIAN 0
#   else
#       define WYHASH_LITTLE_ENDIAN 1
#   endif
#endif

inline uint64_t read8(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if !(WYHASH_LITTLE_ENDIAN)
#   if defined(__GNUC__) || defined(__clang__)
    v = __builtin_bswap64(v);
#   elif defined(_MSC_VER)
    v = _byteswap_uint64(v);
#   else
    v = ((v >> 56) & 0xff)
      | ((v >> 40) & 0xff00)
      | ((v >> 24) & 0xff0000)
      | ((v >>  8) & 0xff000000)
      | ((v <<  8) & 0xff00000000)
      | ((v << 24) & 0xff0000000000)
      | ((v << 40) & 0xff000000000000)
      | ((v << 56) & 0xff00000000000000);
#   endif
#endif
    return v;
}

inline uint64_t read4(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
#if !(WYHASH_LITTLE_ENDIAN)
#   if defined(__GNUC__) || defined(__clang__)
    v = __builtin_bswap32(v);
#   elif defined(_MSC_VER)
    v = _byteswap_ulong(v);
#   else
    v = ((v >> 24) & 0xff)
      | ((v >>  8) & 0xff00)
      | ((v <<  8) & 0xff0000)
      | ((v << 24) & 0xff000000);
#   endif
#endif
    return (uint64_t)v;
}

inline uint64_t read3(const uint8_t* p, size_t k) {
    return (((uint64_t)p[0]) << 16)
         | (((uint64_t)p[k >> 1]) << 8)
         |  (uint64_t)p[k - 1];
}

} // namespace detail

static const uint64_t wyp[4] = {
    0x2d358dccaa6c78a5ull,
    0x8bb84b93962eacc9ull,
    0x4b33a62ed433d4a3ull,
    0x4d5a2da51de1aa47ull
};

static inline uint64_t wyhash(const void* key, size_t len,
                              uint64_t seed, const uint64_t* secret) {
    const uint8_t* p = (const uint8_t*)key;
    seed ^= detail::wymix(seed ^ secret[0], secret[1]);
    uint64_t a, b;
    if (len <= 16) {
        if (len >= 4) {
            a = (detail::read4(p) << 32) | detail::read4(p + ((len >> 3) << 2));
            b = (detail::read4(p + len - 4) << 32)
              | detail::read4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = detail::read3(p, len);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = len;
        if (i >= 48) {
            uint64_t see1 = seed, see2 = seed;
            do {
                seed = detail::wymix(detail::read8(p)     ^ secret[1],
                                     detail::read8(p + 8) ^ seed);
                see1 = detail::wymix(detail::read8(p + 16) ^ secret[2],
                                     detail::read8(p + 24) ^ see1);
                see2 = detail::wymix(detail::read8(p + 32) ^ secret[3],
                                     detail::read8(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while (i >= 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16) {
            seed = detail::wymix(detail::read8(p)     ^ secret[1],
                                 detail::read8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = detail::read8(p + i - 16);
        b = detail::read8(p + i - 8);
    }
    a ^= secret[1];
    b ^= seed;
    detail::wymum(&a, &b);
    return detail::wymix(a ^ secret[0] ^ len, b ^ secret[1]);
}

static inline uint64_t hash(const void* key, size_t len,
                            uint64_t seed = 0) {
    return wyhash(key, len, seed, wyp);
}

static inline uint64_t wyhash64(uint64_t A, uint64_t B) {
    A ^= 0x2d358dccaa6c78a5ull;
    B ^= 0x8bb84b93962eacc9ull;
    detail::wymum(&A, &B);
    return detail::wymix(A ^ 0x2d358dccaa6c78a5ull,
                         B ^ 0x8bb84b93962eacc9ull);
}

static inline uint64_t wyrand(uint64_t* seed) {
    *seed += 0x2d358dccaa6c78a5ull;
    return detail::wymix(*seed, *seed ^ 0x8bb84b93962eacc9ull);
}

static inline double wy2u01(uint64_t r) {
    const double norm = 1.0 / (1ull << 52);
    return (double)(r >> 12) * norm;
}

static inline double wy2gau(uint64_t r) {
    const double norm = 1.0 / (1ull << 20);
    return (double)((r & 0x1fffff)
                  + ((r >> 21) & 0x1fffff)
                  + ((r >> 42) & 0x1fffff)) * norm - 3.0;
}

static inline uint64_t wy2u0k(uint64_t r, uint64_t k) {
    detail::wymum(&r, &k);
    return k;
}

template <typename T>
static inline uint64_t hash_of(const T& value, uint64_t seed = 0) {
    return hash(&value, sizeof(T), seed);
}

static inline uint64_t hash_cstr(const char* s, uint64_t seed = 0) {
    if (s == nullptr) {
        return hash("", 0, seed);
    }
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return hash(s, n, seed);
}

} } // namespace df::wy