#pragma once

// bc_bit_array.h
//
// Runtime-sized bitset. Dynamic-length bit-granular storage, 64-byte
// cache-line aligned, allocated through df::allocate. Same find operations
// as df::bitset<N>, all intrinsic-based, but not constexpr (allocation).
//
// Style: snake_case, One True Brace Style, bare size_t / ptrdiff_t / uint64_t.

#include "bc_bit_ops.h"
#include "bc_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <utility>

namespace df {

class bit_array {
public:
    bit_array() noexcept = default;

    explicit bit_array(ptrdiff_t bit_count) {
        resize(bit_count);
    }

    bit_array(const bit_array&) = delete;
    bit_array& operator=(const bit_array&) = delete;

    bit_array(bit_array&& other) noexcept
        : words_(other.words_), len_(other.len_), word_count_(other.word_count_) {
        other.words_ = nullptr;
        other.len_ = 0;
        other.word_count_ = 0;
    }

    bit_array& operator=(bit_array&& other) noexcept {
        if (this != &other) {
            release();
            words_ = other.words_;
            len_ = other.len_;
            word_count_ = other.word_count_;
            other.words_ = nullptr;
            other.len_ = 0;
            other.word_count_ = 0;
        }
        return *this;
    }

    ~bit_array() {
        release();
    }

    void resize(ptrdiff_t bit_count) {
        assert(bit_count >= 0);
        if (bit_count == len_) {
            return;
        }
        const ptrdiff_t new_word_count = (bit_count + 63) / 64;
        if (new_word_count == word_count_) {
            len_ = bit_count;
            mask_last_word();
            return;
        }
        uint64_t* fresh = nullptr;
        if (new_word_count > 0) {
            void* raw = df::allocate(static_cast<size_t>(new_word_count) * sizeof(uint64_t));
            assert(raw != nullptr && "bit_array::resize allocation failed");
            fresh = static_cast<uint64_t*>(raw);
            memset(fresh, 0, static_cast<size_t>(new_word_count) * sizeof(uint64_t));
            if (words_ != nullptr && word_count_ > 0) {
                const ptrdiff_t copy_words = (word_count_ < new_word_count) ? word_count_ : new_word_count;
                memcpy(fresh, words_, static_cast<size_t>(copy_words) * sizeof(uint64_t));
            }
        }
        if (words_ != nullptr) {
            df::deallocate(words_, static_cast<size_t>(word_count_) * sizeof(uint64_t));
        }
        words_ = fresh;
        len_ = bit_count;
        word_count_ = new_word_count;
        mask_last_word();
    }

    void reserve(ptrdiff_t bit_count) {
        assert(bit_count >= 0);
        const ptrdiff_t want_words = (bit_count + 63) / 64;
        if (want_words <= word_count_) {
            return;
        }
        void* raw = df::allocate(static_cast<size_t>(want_words) * sizeof(uint64_t));
        assert(raw != nullptr && "bit_array::reserve allocation failed");
        uint64_t* fresh = static_cast<uint64_t*>(raw);
        memset(fresh, 0, static_cast<size_t>(want_words) * sizeof(uint64_t));
        if (words_ != nullptr && word_count_ > 0) {
            memcpy(fresh, words_, static_cast<size_t>(word_count_) * sizeof(uint64_t));
        }
        if (words_ != nullptr) {
            df::deallocate(words_, static_cast<size_t>(word_count_) * sizeof(uint64_t));
        }
        words_ = fresh;
        word_count_ = want_words;
    }

    void shrink_to_fit() {
        const ptrdiff_t want_words = (len_ + 63) / 64;
        if (want_words == word_count_) {
            return;
        }
        if (want_words == 0) {
            release();
            return;
        }
        void* raw = df::allocate(static_cast<size_t>(want_words) * sizeof(uint64_t));
        assert(raw != nullptr && "bit_array::shrink_to_fit allocation failed");
        uint64_t* fresh = static_cast<uint64_t*>(raw);
        memset(fresh, 0, static_cast<size_t>(want_words) * sizeof(uint64_t));
        const ptrdiff_t copy_words = (word_count_ < want_words) ? word_count_ : want_words;
        if (copy_words > 0 && words_ != nullptr) {
            memcpy(fresh, words_, static_cast<size_t>(copy_words) * sizeof(uint64_t));
        }
        if (words_ != nullptr) {
            df::deallocate(words_, static_cast<size_t>(word_count_) * sizeof(uint64_t));
        }
        words_ = fresh;
        word_count_ = want_words;
        mask_last_word();
    }

    void release() noexcept {
        if (words_ != nullptr) {
            df::deallocate(words_, static_cast<size_t>(word_count_) * sizeof(uint64_t));
            words_ = nullptr;
        }
        len_ = 0;
        word_count_ = 0;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(len_);
    }

    ptrdiff_t ssize() const noexcept {
        return len_;
    }

    ptrdiff_t scapacity() const noexcept {
        return word_count_ * 64;
    }

    size_t capacity() const noexcept {
        return static_cast<size_t>(word_count_ * 64);
    }

    bool empty() const noexcept {
        return len_ == 0;
    }

    bool test(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len_ && "bit_array::test out of range");
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    bool operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len_);
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    void set(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len_ && "bit_array::set out of range");
        words_[i >> 6] |= (uint64_t(1) << (i & 63));
    }

    void clear(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len_ && "bit_array::clear out of range");
        words_[i >> 6] &= ~(uint64_t(1) << (i & 63));
    }

    void toggle(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len_ && "bit_array::toggle out of range");
        words_[i >> 6] ^= (uint64_t(1) << (i & 63));
    }

    void set_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            words_[w] = ~uint64_t(0);
        }
        mask_last_word();
    }

    void clear_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            words_[w] = 0;
        }
    }

    void flip_all() noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            words_[w] = ~words_[w];
        }
        mask_last_word();
    }

    ptrdiff_t count() const noexcept {
        ptrdiff_t total = 0;
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            total += static_cast<ptrdiff_t>(popcount64(words_[w]));
        }
        return total;
    }

    bool any() const noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            if (words_[w] != 0) {
                return true;
            }
        }
        return false;
    }

    bool none() const noexcept {
        return !any();
    }

    bool all() const noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            uint64_t expected = ~uint64_t(0);
            if (w == word_count_ - 1) {
                expected = last_word_mask();
            }
            if (words_[w] != expected) {
                return false;
            }
        }
        return true;
    }

    ptrdiff_t find_first_set() const noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            if (words_[w] != 0) {
                return w * 64 + static_cast<ptrdiff_t>(ctz64(words_[w]));
            }
        }
        return -1;
    }

    ptrdiff_t find_first_unset() const noexcept {
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            uint64_t word = ~words_[w];
            if (w == word_count_ - 1) {
                word &= last_word_mask();
            }
            if (word != 0) {
                return w * 64 + static_cast<ptrdiff_t>(ctz64(word));
            }
        }
        return -1;
    }

    ptrdiff_t find_next_set(ptrdiff_t from) const noexcept {
        if (from >= len_) {
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
        for (ptrdiff_t i = w + 1; i < word_count_; ++i) {
            if (words_[i] != 0) {
                return i * 64 + static_cast<ptrdiff_t>(ctz64(words_[i]));
            }
        }
        return -1;
    }

    ptrdiff_t find_next_unset(ptrdiff_t from) const noexcept {
        if (from >= len_) {
            return -1;
        }
        if (from < 0) {
            from = 0;
        }
        ptrdiff_t w = from >> 6;
        uint64_t word = ~words_[w] >> (from & 63);
        if (w == word_count_ - 1) {
            word &= last_word_mask() >> (from & 63);
        }
        if (word != 0) {
            return from + static_cast<ptrdiff_t>(ctz64(word));
        }
        for (ptrdiff_t i = w + 1; i < word_count_; ++i) {
            uint64_t nw = ~words_[i];
            if (i == word_count_ - 1) {
                nw &= last_word_mask();
            }
            if (nw != 0) {
                return i * 64 + static_cast<ptrdiff_t>(ctz64(nw));
            }
        }
        return -1;
    }

    bool operator==(const bit_array& other) const noexcept {
        if (len_ != other.len_) {
            return false;
        }
        for (ptrdiff_t w = 0; w < word_count_; ++w) {
            if (words_[w] != other.words_[w]) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const bit_array& other) const noexcept {
        return !(*this == other);
    }

    void swap(bit_array& other) noexcept {
        std::swap(words_,      other.words_);
        std::swap(len_,        other.len_);
        std::swap(word_count_, other.word_count_);
    }

    uint64_t* words_ = nullptr;

private:
    ptrdiff_t len_ = 0;
    ptrdiff_t word_count_ = 0;

    void mask_last_word() noexcept {
        if (word_count_ > 0 && (len_ % 64) != 0) {
            words_[word_count_ - 1] &= last_word_mask();
        }
    }

    uint64_t last_word_mask() const noexcept {
        if ((len_ % 64) == 0) {
            return ~uint64_t(0);
        } else {
            return (uint64_t(1) << (len_ % 64)) - 1;
        }
    }
};

inline void swap(bit_array& a, bit_array& b) noexcept {
    a.swap(b);
}

} // namespace df