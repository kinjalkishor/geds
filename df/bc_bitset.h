#pragma once

// bc_bitset.h
//
// Fixed-size bitset, compile-time N, constexpr-friendly, aligned to 64
// bytes. Portable complement to std::bitset<N>.
//
// Left/right shift operators mirror std::bitset<N>:
//   - bits shifted past the last bit are dropped
//   - vacated bits are zero-filled
//   - the last word is re-masked after every shift
//
// Style: snake_case, One True Brace Style, template param N.

#include "bc_bit_ops.h"

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

namespace df {

template <ptrdiff_t N>
class bitset {
    static_assert(N >= 0, "bitset size must be non-negative");

public:
    static constexpr ptrdiff_t bit_count = N;
    static constexpr ptrdiff_t word_count = (N + 63) / 64;

    constexpr bitset() noexcept : words_{} {}

    constexpr bitset(uint64_t value) noexcept : words_{} {
        if constexpr (word_count > 0) {
            words_[0] = value;
            mask_last_word();
        }
    }

    constexpr size_t size() const noexcept {
        return static_cast<size_t>(N);
    }

    constexpr ptrdiff_t ssize() const noexcept {
        return N;
    }

    constexpr bool empty() const noexcept {
        return N == 0;
    }

    constexpr bool test(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < N && "bitset::test out of range");
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    constexpr bool operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < N);
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    constexpr void set(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < N && "bitset::set out of range");
        words_[i >> 6] |= (uint64_t(1) << (i & 63));
    }

    constexpr void clear(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < N && "bitset::clear out of range");
        words_[i >> 6] &= ~(uint64_t(1) << (i & 63));
    }

    constexpr void toggle(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < N && "bitset::toggle out of range");
        words_[i >> 6] ^= (uint64_t(1) << (i & 63));
    }

    constexpr void set_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] = ~uint64_t(0);
        }
        mask_last_word();
    }

    constexpr void clear_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] = 0;
        }
    }

    constexpr void flip_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] = ~words_[w];
        }
        mask_last_word();
    }

    constexpr ptrdiff_t count() const noexcept {
        ptrdiff_t total = 0;
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            total += static_cast<ptrdiff_t>(popcount64(words_[w]));
        }
        return total;
    }

    constexpr bool any() const noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            if (words_[w] != 0) {
                return true;
            }
        }
        return false;
    }

    constexpr bool none() const noexcept {
        return !any();
    }

    constexpr bool all() const noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            uint64_t expected = ~uint64_t(0);
            if (w == word_count - 1) {
                expected = last_word_mask();
            }
            if (words_[w] != expected) {
                return false;
            }
        }
        return true;
    }

    constexpr ptrdiff_t find_first_set() const noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            if (words_[w] != 0) {
                return w * 64 + static_cast<ptrdiff_t>(ctz64(words_[w]));
            }
        }
        return -1;
    }

    constexpr ptrdiff_t find_first_unset() const noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            uint64_t word = ~words_[w];
            if (w == word_count - 1) {
                word &= last_word_mask();
            }
            if (word != 0) {
                return w * 64 + static_cast<ptrdiff_t>(ctz64(word));
            }
        }
        return -1;
    }

    constexpr ptrdiff_t find_next_set(ptrdiff_t from) const noexcept {
        if (from >= N) {
            return -1;
        }
        if (from < 0) {
            from = 0;
        }
        ptrdiff_t w = from >> 6;
        uint64_t word = words_[w] >> (from & 63);
        if (word != 0) {
            return from + static_cast<ptrdiff_t>(ctz64(word));
        }
        for (ptrdiff_t i = w + 1; i < word_count; ++i) {
            if (words_[i] != 0) {
                return i * 64 + static_cast<ptrdiff_t>(ctz64(words_[i]));
            }
        }
        return -1;
    }

    constexpr ptrdiff_t find_next_unset(ptrdiff_t from) const noexcept {
        if (from >= N) {
            return -1;
        }
        if (from < 0) {
            from = 0;
        }
        ptrdiff_t w = from >> 6;
        uint64_t word = ~words_[w] >> (from & 63);
        if (w == word_count - 1) {
            word &= last_word_mask() >> (from & 63);
        }
        if (word != 0) {
            return from + static_cast<ptrdiff_t>(ctz64(word));
        }
        for (ptrdiff_t i = w + 1; i < word_count; ++i) {
            uint64_t nw = ~words_[i];
            if (i == word_count - 1) {
                nw &= last_word_mask();
            }
            if (nw != 0) {
                return i * 64 + static_cast<ptrdiff_t>(ctz64(nw));
            }
        }
        return -1;
    }

    constexpr bitset& operator&=(const bitset& other) noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] &= other.words_[w];
        }
        return *this;
    }

    constexpr bitset& operator|=(const bitset& other) noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] |= other.words_[w];
        }
        return *this;
    }

    constexpr bitset& operator^=(const bitset& other) noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            words_[w] ^= other.words_[w];
        }
        return *this;
    }

    constexpr bitset& operator<<=(ptrdiff_t shift) noexcept {
        if (shift <= 0 || N == 0) {
            return *this;
        }
        if (shift >= N) {
            clear_all();
            return *this;
        }
        const ptrdiff_t word_shift = shift >> 6;
        const ptrdiff_t bit_shift  = shift & 63;
        for (ptrdiff_t w = word_count - 1; w >= 0; --w) {
            const ptrdiff_t src = w - word_shift;
            const uint64_t lo = (src     >= 0) ? words_[src] : 0;
            const uint64_t hi = (src - 1 >= 0) ? words_[src - 1] : 0;
            if (bit_shift == 0) {
                words_[w] = lo;
            } else {
                words_[w] = (lo << bit_shift) | (hi >> (64 - bit_shift));
            }
        }
        mask_last_word();
        return *this;
    }

    constexpr bitset& operator>>=(ptrdiff_t shift) noexcept {
        if (shift <= 0 || N == 0) {
            return *this;
        }
        if (shift >= N) {
            clear_all();
            return *this;
        }
        const ptrdiff_t word_shift = shift >> 6;
        const ptrdiff_t bit_shift  = shift & 63;
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            const ptrdiff_t src = w + word_shift;
            const uint64_t lo = (src     < word_count) ? words_[src] : 0;
            const uint64_t hi = (src + 1 < word_count) ? words_[src + 1] : 0;
            if (bit_shift == 0) {
                words_[w] = lo;
            } else {
                words_[w] = (lo >> bit_shift) | (hi << (64 - bit_shift));
            }
        }
        mask_last_word();
        return *this;
    }

    constexpr bitset operator~() const noexcept {
        bitset result = *this;
        result.flip_all();
        return result;
    }

    friend constexpr bitset operator&(bitset a, const bitset& b) noexcept {
        a &= b;
        return a;
    }

    friend constexpr bitset operator|(bitset a, const bitset& b) noexcept {
        a |= b;
        return a;
    }

    friend constexpr bitset operator^(bitset a, const bitset& b) noexcept {
        a ^= b;
        return a;
    }

    friend constexpr bitset operator<<(bitset a, ptrdiff_t shift) noexcept {
        a <<= shift;
        return a;
    }

    friend constexpr bitset operator>>(bitset a, ptrdiff_t shift) noexcept {
        a >>= shift;
        return a;
    }

    constexpr bool operator==(const bitset& other) const noexcept {
        for (ptrdiff_t w = 0; w < word_count; ++w) {
            if (words_[w] != other.words_[w]) {
                return false;
            }
        }
        return true;
    }

    constexpr bool operator!=(const bitset& other) const noexcept {
        return !(*this == other);
    }

    alignas(64) uint64_t words_[word_count > 0 ? word_count : 1] = {};

private:
    constexpr void mask_last_word() noexcept {
        if constexpr (word_count > 0 && (N % 64) != 0) {
            words_[word_count - 1] &= last_word_mask();
        }
    }

    static constexpr uint64_t last_word_mask() noexcept {
        if constexpr ((N % 64) == 0) {
            return ~uint64_t(0);
        } else {
            return (uint64_t(1) << (N % 64)) - 1;
        }
    }
};

} // namespace df