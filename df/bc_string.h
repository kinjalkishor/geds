#pragma once

// bc_string.h
//
// Lightweight templated basic_string<CharT> for the game engine.
// - member names: ptr, len, cap  (cap counts the null slot internally)
// - always null-terminated: ptr[len] == CharT(0)
// - capacity() / scapacity() return usable chars only (cap - 1)
// - allocates through df::allocate / df::deallocate / df::reallocate
// - no allocator template parameter; template only on CharT
// - ptrdiff_t internal size; size() unsigned, ssize() signed
// - std::basic_string-compatible API where it matters, plus a few helpers
// - no SSO
// - starts_with / ends_with / contains / replace added (C++20/23 parity)
// - append(n, c) / assign(n, c) added (std::string parity)
// - snake_case, One True Brace Style, template param CharT

#include "bc_allocator.h"

#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#include <utility>
#include <type_traits>

namespace df {

template <typename CharT>
class basic_string {
public:
    using value_type      = CharT;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = CharT&;
    using const_reference = const CharT&;
    using pointer         = CharT*;
    using const_pointer   = const CharT*;
    using iterator        = CharT*;
    using const_iterator  = const CharT*;

    static constexpr ptrdiff_t min_cap = 16;
    static constexpr ptrdiff_t npos    = -1;

    basic_string() noexcept : ptr(nullptr), len(0), cap(0) {
        ensure_null_slot();
    }

    basic_string(const CharT* cstr) : ptr(nullptr), len(0), cap(0) {
        if (cstr == nullptr) {
            ensure_null_slot();
            return;
        }
        const ptrdiff_t n = static_cast<ptrdiff_t>(c_str_len(cstr));
        reserve(n);
        if (n > 0) {
            memcpy(ptr, cstr, static_cast<size_t>(n) * sizeof(CharT));
        }
        len = n;
        ptr[len] = CharT(0);
    }

    basic_string(const CharT* data, ptrdiff_t n) : ptr(nullptr), len(0), cap(0) {
        assert(n >= 0);
        if (n == 0) {
            ensure_null_slot();
            return;
        }
        reserve(n);
        if (data != nullptr) {
            memcpy(ptr, data, static_cast<size_t>(n) * sizeof(CharT));
        } else {
            memset(ptr, 0, static_cast<size_t>(n) * sizeof(CharT));
        }
        len = n;
        ptr[len] = CharT(0);
    }

    basic_string(ptrdiff_t n, CharT c) : ptr(nullptr), len(0), cap(0) {
        assert(n >= 0);
        if (n == 0) {
            ensure_null_slot();
            return;
        }
        reserve(n);
        for (ptrdiff_t i = 0; i < n; ++i) {
            ptr[i] = c;
        }
        len = n;
        ptr[len] = CharT(0);
    }

    basic_string(const basic_string& other) : ptr(nullptr), len(0), cap(0) {
        if (other.len > 0) {
            reserve(other.len);
            memcpy(ptr, other.ptr, static_cast<size_t>(other.len) * sizeof(CharT));
        }
        len = other.len;
        ptr[len] = CharT(0);
    }

    basic_string(basic_string&& other) noexcept : ptr(other.ptr), len(other.len), cap(other.cap) {
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
    }

    basic_string& operator=(const basic_string& other) {
        if (this == &other) {
            return *this;
        }
        basic_string tmp(other);
        swap(tmp);
        return *this;
    }

    basic_string& operator=(basic_string&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(CharT));
        }
        ptr = other.ptr;
        len = other.len;
        cap = other.cap;
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
        return *this;
    }

    basic_string& operator=(const CharT* cstr) {
        assign(cstr);
        return *this;
    }

    ~basic_string() {
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(CharT));
        }
    }

    CharT& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }
    const CharT& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    CharT& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len && "basic_string::at out of range");
        return ptr[i];
    }
    const CharT& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "basic_string::at out of range");
        return ptr[i];
    }

    CharT& front() noexcept { assert(len > 0); return ptr[0]; }
    const CharT& front() const noexcept { assert(len > 0); return ptr[0]; }

    CharT& back() noexcept { assert(len > 0); return ptr[len - 1]; }
    const CharT& back() const noexcept { assert(len > 0); return ptr[len - 1]; }

    const CharT* c_str() const noexcept { return ptr ? ptr : empty_literal(); }
    CharT* data() noexcept { return ptr ? ptr : const_cast<CharT*>(empty_literal()); }
    const CharT* data() const noexcept { return ptr ? ptr : empty_literal(); }

    iterator begin() noexcept { return ptr; }
    iterator end()   noexcept { return ptr + len; }
    const_iterator begin() const noexcept { return ptr; }
    const_iterator end()   const noexcept { return ptr + len; }
    const_iterator cbegin() const noexcept { return ptr; }
    const_iterator cend()   const noexcept { return ptr + len; }

    size_t size() const noexcept { return static_cast<size_t>(len); }
    ptrdiff_t ssize() const noexcept { return len; }

    size_t capacity() const noexcept {
        return cap > 0 ? static_cast<size_t>(cap - 1) : 0;
    }
    ptrdiff_t scapacity() const noexcept {
        return cap > 0 ? (cap - 1) : 0;
    }

    bool empty() const noexcept { return len == 0; }

    void reserve(ptrdiff_t n) {
        assert(n >= 0);
        const ptrdiff_t need = n + 1;
        if (need <= cap) {
            return;
        }
        reallocate_to(need);
    }

    void shrink_to_fit() {
        const ptrdiff_t need = len + 1;
        if (cap > need) {
            reallocate_to(need);
        }
    }

    void resize(ptrdiff_t new_len) {
        resize(new_len, CharT(0));
    }

    void resize(ptrdiff_t new_len, CharT c) {
        assert(new_len >= 0);
        if (new_len == len) {
            return;
        }
        if (new_len > len) {
            if (new_len + 1 > cap) {
                reserve(grow_to(new_len));
            }
            for (ptrdiff_t i = len; i < new_len; ++i) {
                ptr[i] = c;
            }
            len = new_len;
            ptr[len] = CharT(0);
        } else {
            len = new_len;
            ptr[len] = CharT(0);
        }
    }

    void clear() noexcept {
        len = 0;
        if (ptr != nullptr) {
            ptr[0] = CharT(0);
        }
    }

    void push_back(CharT c) {
        if (len + 1 >= cap) {
            reserve(grow_to(len + 1));
        }
        ptr[len] = c;
        ++len;
        ptr[len] = CharT(0);
    }

    void pop_back() noexcept {
        assert(len > 0);
        --len;
        ptr[len] = CharT(0);
    }

    void append(const CharT* cstr, ptrdiff_t n) {
        assert(n >= 0);
        if (n == 0) {
            return;
        }
        const ptrdiff_t need = len + n;
        if (need + 1 > cap) {
            reserve(grow_to(need));
        }
        memcpy(ptr + len, cstr, static_cast<size_t>(n) * sizeof(CharT));
        len = need;
        ptr[len] = CharT(0);
    }

    void append(const CharT* cstr) {
        if (cstr == nullptr) {
            return;
        }
        append(cstr, static_cast<ptrdiff_t>(c_str_len(cstr)));
    }

    void append(const basic_string& other) {
        append(other.ptr, other.len);
    }

    void append(ptrdiff_t n, CharT c) {
        assert(n >= 0);
        if (n == 0) {
            return;
        }
        const ptrdiff_t need = len + n;
        if (need + 1 > cap) {
            reserve(grow_to(need));
        }
        for (ptrdiff_t i = 0; i < n; ++i) {
            ptr[len + i] = c;
        }
        len = need;
        ptr[len] = CharT(0);
    }

    basic_string& operator+=(const CharT* cstr) { append(cstr); return *this; }
    basic_string& operator+=(const basic_string& other) { append(other); return *this; }
    basic_string& operator+=(CharT c) { push_back(c); return *this; }

    void assign(const CharT* cstr, ptrdiff_t n) {
        assert(n >= 0);
        clear();
        append(cstr, n);
    }

    void assign(const CharT* cstr) {
        clear();
        append(cstr);
    }

    void assign(const basic_string& other) {
        if (this == &other) {
            return;
        }
        clear();
        append(other);
    }

    void assign(ptrdiff_t n, CharT c) {
        clear();
        append(n, c);
    }

    void insert(ptrdiff_t pos, const CharT* cstr, ptrdiff_t n) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0);
        if (n == 0) {
            return;
        }
        const ptrdiff_t need = len + n;
        if (need + 1 > cap) {
            reserve(grow_to(need));
        }
        memmove(ptr + pos + n,
                ptr + pos,
                static_cast<size_t>(len - pos) * sizeof(CharT));
        memcpy(ptr + pos, cstr, static_cast<size_t>(n) * sizeof(CharT));
        len = need;
        ptr[len] = CharT(0);
    }

    void insert(ptrdiff_t pos, CharT c) {
        insert(pos, &c, 1);
    }

    void erase(ptrdiff_t pos, ptrdiff_t n) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0 && pos + n <= len);
        if (n == 0) {
            return;
        }
        const ptrdiff_t tail = len - pos - n;
        if (tail > 0) {
            memmove(ptr + pos, ptr + pos + n,
                    static_cast<size_t>(tail) * sizeof(CharT));
        }
        len -= n;
        ptr[len] = CharT(0);
    }

    void erase(ptrdiff_t pos) { erase(pos, 1); }

    void replace(ptrdiff_t pos, ptrdiff_t n, const CharT* s, ptrdiff_t m) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0);
        assert(m >= 0);
        assert(s != nullptr || m == 0);
        if (pos + n > len) {
            n = len - pos;
        }
        const ptrdiff_t removed_tail = len - pos - n;
        const ptrdiff_t new_len = pos + m + removed_tail;
        if (new_len + 1 > cap) {
            reserve(grow_to(new_len));
        }
        if (removed_tail > 0 && n != m) {
            memmove(ptr + pos + m, ptr + pos + n,
                    static_cast<size_t>(removed_tail) * sizeof(CharT));
        }
        if (m > 0) {
            memmove(ptr + pos, s, static_cast<size_t>(m) * sizeof(CharT));
        }
        len = new_len;
        ptr[len] = CharT(0);
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, const CharT* s) {
        if (s == nullptr) {
            replace(pos, n, nullptr, 0);
            return;
        }
        replace(pos, n, s, static_cast<ptrdiff_t>(c_str_len(s)));
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, const basic_string& other) {
        replace(pos, n, other.ptr, other.len);
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, CharT c) {
        replace(pos, n, &c, 1);
    }

    basic_string substr(ptrdiff_t pos, ptrdiff_t n = npos) const {
        assert(pos >= 0 && pos <= len);
        ptrdiff_t count = (n == npos || pos + n > len) ? (len - pos) : n;
        if (count < 0) {
            count = 0;
        }
        return basic_string(ptr + pos, count);
    }

    int compare(const basic_string& other) const noexcept {
        const ptrdiff_t n = (len < other.len) ? len : other.len;
        if (n > 0) {
            const int r = compare_chars(ptr, other.ptr, n);
            if (r != 0) {
                return r;
            }
        }
        if (len < other.len) return -1;
        if (len > other.len) return 1;
        return 0;
    }

    int compare(const CharT* cstr) const noexcept {
        if (cstr == nullptr) {
            return len == 0 ? 0 : 1;
        }
        const ptrdiff_t other_len = static_cast<ptrdiff_t>(c_str_len(cstr));
        const ptrdiff_t n = (len < other_len) ? len : other_len;
        if (n > 0) {
            const int r = compare_chars(ptr, cstr, n);
            if (r != 0) {
                return r;
            }
        }
        if (len < other_len) return -1;
        if (len > other_len) return 1;
        return 0;
    }

    ptrdiff_t find(CharT c, ptrdiff_t start = 0) const noexcept {
        if (start < 0 || start > len) {
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            const void* hit = memchr(ptr + start,
                                     static_cast<unsigned char>(c),
                                     static_cast<size_t>(len - start));
            if (hit == nullptr) {
                return npos;
            }
            return static_cast<ptrdiff_t>(static_cast<const CharT*>(hit) - ptr);
        } else {
            for (ptrdiff_t i = start; i < len; ++i) {
                if (ptr[i] == c) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t find(const CharT* needle, ptrdiff_t start = 0) const noexcept {
        if (needle == nullptr) {
            return npos;
        }
        return find(needle, static_cast<ptrdiff_t>(c_str_len(needle)), start);
    }

    ptrdiff_t find(const CharT* needle,
                   ptrdiff_t needle_len,
                   ptrdiff_t start) const noexcept {
        if (needle == nullptr || needle_len <= 0) {
            return start <= len ? start : npos;
        }
        if (start < 0 || start > len) {
            return npos;
        }
        if (needle_len > len - start) {
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            const CharT first = needle[0];
            const ptrdiff_t last_possible = len - needle_len;
            ptrdiff_t cur = start;
            while (cur <= last_possible) {
                const void* hit = memchr(ptr + cur,
                                         static_cast<unsigned char>(first),
                                         static_cast<size_t>(last_possible - cur + 1));
                if (hit == nullptr) {
                    return npos;
                }
                const CharT* h = static_cast<const CharT*>(hit);
                if (memcmp(h, needle,
                           static_cast<size_t>(needle_len) * sizeof(CharT)) == 0) {
                    return static_cast<ptrdiff_t>(h - ptr);
                }
                cur = static_cast<ptrdiff_t>(h - ptr) + 1;
            }
            return npos;
        } else {
            const ptrdiff_t last_possible = len - needle_len;
            for (ptrdiff_t i = start; i <= last_possible; ++i) {
                if (compare_chars(ptr + i, needle, needle_len) == 0) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t find(const basic_string& needle, ptrdiff_t start = 0) const noexcept {
        return find(needle.ptr, needle.len, start);
    }

    ptrdiff_t rfind(CharT c, ptrdiff_t start = npos) const noexcept {
        if (len == 0) {
            return npos;
        }
        if (start == npos || start >= len) {
            start = len - 1;
        }
        if (start < 0) {
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            const void* hit = mem_rchr(ptr, static_cast<unsigned char>(c),
                                       static_cast<size_t>(start + 1));
            if (hit == nullptr) {
                return npos;
            }
            return static_cast<ptrdiff_t>(static_cast<const CharT*>(hit) - ptr);
        } else {
            for (ptrdiff_t i = start; i >= 0; --i) {
                if (ptr[i] == c) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t rfind(const CharT* needle, ptrdiff_t start = npos) const noexcept {
        if (needle == nullptr) {
            return npos;
        }
        return rfind(needle, static_cast<ptrdiff_t>(c_str_len(needle)), start);
    }

    ptrdiff_t rfind(const CharT* needle, ptrdiff_t needle_len,
                    ptrdiff_t start = npos) const noexcept {
        if (needle == nullptr || needle_len <= 0) {
            if (start == npos || start >= len) {
                return len;
            }
            return start >= 0 ? start : npos;
        }
        if (needle_len > len) {
            return npos;
        }
        if (start == npos || start > len - needle_len) {
            start = len - needle_len;
        }
        if (start < 0) {
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            const CharT first = needle[0];
            ptrdiff_t cur = start;
            while (cur >= 0) {
                const void* hit = mem_rchr(ptr, static_cast<unsigned char>(first),
                                           static_cast<size_t>(cur + 1));
                if (hit == nullptr) {
                    return npos;
                }
                const CharT* h = static_cast<const CharT*>(hit);
                if (memcmp(h, needle,
                           static_cast<size_t>(needle_len) * sizeof(CharT)) == 0) {
                    return static_cast<ptrdiff_t>(h - ptr);
                }
                cur = static_cast<ptrdiff_t>(h - ptr) - 1;
            }
            return npos;
        } else {
            for (ptrdiff_t i = start; i >= 0; --i) {
                if (compare_chars(ptr + i, needle, needle_len) == 0) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t rfind(const basic_string& needle, ptrdiff_t start = npos) const noexcept {
        return rfind(needle.ptr, needle.len, start);
    }

    ptrdiff_t find_first_of(const CharT* set, ptrdiff_t set_len,
                            ptrdiff_t start = 0) const noexcept {
        if (set == nullptr || set_len <= 0) {
            return npos;
        }
        if (start < 0 || start > len) {
            return npos;
        }
        if (set_len <= 8) {
            ptrdiff_t best = npos;
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                const ptrdiff_t hit = find(set[j], start);
                if (hit != npos && (best == npos || hit < best)) {
                    best = hit;
                }
            }
            return best;
        }
        if constexpr (sizeof(CharT) == 1) {
            unsigned char map[256] = {};
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                map[static_cast<unsigned char>(set[j])] = 1;
            }
            for (ptrdiff_t i = start; i < len; ++i) {
                if (map[static_cast<unsigned char>(ptr[i])]) {
                    return i;
                }
            }
            return npos;
        } else {
            for (ptrdiff_t i = start; i < len; ++i) {
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        return i;
                    }
                }
            }
            return npos;
        }
    }

    ptrdiff_t find_first_of(const CharT* set, ptrdiff_t start = 0) const noexcept {
        if (set == nullptr) {
            return npos;
        }
        return find_first_of(set, static_cast<ptrdiff_t>(c_str_len(set)), start);
    }

    ptrdiff_t find_first_of(const basic_string& set, ptrdiff_t start = 0) const noexcept {
        return find_first_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_last_of(const CharT* set, ptrdiff_t set_len,
                           ptrdiff_t start = npos) const noexcept {
        if (set == nullptr || set_len <= 0) {
            return npos;
        }
        if (len == 0) {
            return npos;
        }
        if (start == npos || start >= len) {
            start = len - 1;
        }
        if (start < 0) {
            return npos;
        }
        if (set_len <= 8) {
            ptrdiff_t best = npos;
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                const ptrdiff_t hit = rfind(set[j], start);
                if (hit != npos && (best == npos || hit > best)) {
                    best = hit;
                }
            }
            return best;
        }
        if constexpr (sizeof(CharT) == 1) {
            unsigned char map[256] = {};
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                map[static_cast<unsigned char>(set[j])] = 1;
            }
            for (ptrdiff_t i = start; i >= 0; --i) {
                if (map[static_cast<unsigned char>(ptr[i])]) {
                    return i;
                }
            }
            return npos;
        } else {
            for (ptrdiff_t i = start; i >= 0; --i) {
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        return i;
                    }
                }
            }
            return npos;
        }
    }

    ptrdiff_t find_last_of(const CharT* set, ptrdiff_t start = npos) const noexcept {
        if (set == nullptr) {
            return npos;
        }
        return find_last_of(set, static_cast<ptrdiff_t>(c_str_len(set)), start);
    }

    ptrdiff_t find_last_of(const basic_string& set, ptrdiff_t start = npos) const noexcept {
        return find_last_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_first_not_of(const CharT* set, ptrdiff_t set_len,
                                ptrdiff_t start = 0) const noexcept {
        if (start < 0 || start > len) {
            return npos;
        }
        if (set == nullptr || set_len <= 0) {
            return start < len ? start : npos;
        }
        if (set_len <= 8) {
            for (ptrdiff_t i = start; i < len; ++i) {
                bool found = false;
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return i;
                }
            }
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            unsigned char map[256] = {};
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                map[static_cast<unsigned char>(set[j])] = 1;
            }
            for (ptrdiff_t i = start; i < len; ++i) {
                if (!map[static_cast<unsigned char>(ptr[i])]) {
                    return i;
                }
            }
            return npos;
        } else {
            for (ptrdiff_t i = start; i < len; ++i) {
                bool found = false;
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t find_first_not_of(const CharT* set, ptrdiff_t start = 0) const noexcept {
        if (set == nullptr) {
            return start < len ? start : npos;
        }
        return find_first_not_of(set, static_cast<ptrdiff_t>(c_str_len(set)), start);
    }

    ptrdiff_t find_first_not_of(const basic_string& set, ptrdiff_t start = 0) const noexcept {
        return find_first_not_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_last_not_of(const CharT* set, ptrdiff_t set_len,
                               ptrdiff_t start = npos) const noexcept {
        if (len == 0) {
            return npos;
        }
        if (start == npos || start >= len) {
            start = len - 1;
        }
        if (start < 0) {
            return npos;
        }
        if (set == nullptr || set_len <= 0) {
            return start;
        }
        if (set_len <= 8) {
            for (ptrdiff_t i = start; i >= 0; --i) {
                bool found = false;
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return i;
                }
            }
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            unsigned char map[256] = {};
            for (ptrdiff_t j = 0; j < set_len; ++j) {
                map[static_cast<unsigned char>(set[j])] = 1;
            }
            for (ptrdiff_t i = start; i >= 0; --i) {
                if (!map[static_cast<unsigned char>(ptr[i])]) {
                    return i;
                }
            }
            return npos;
        } else {
            for (ptrdiff_t i = start; i >= 0; --i) {
                bool found = false;
                for (ptrdiff_t j = 0; j < set_len; ++j) {
                    if (ptr[i] == set[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return i;
                }
            }
            return npos;
        }
    }

    ptrdiff_t find_last_not_of(const CharT* set, ptrdiff_t start = npos) const noexcept {
        if (set == nullptr) {
            if (len == 0) {
                return npos;
            }
            if (start == npos || start >= len) {
                start = len - 1;
            }
            return start >= 0 ? start : npos;
        }
        return find_last_not_of(set, static_cast<ptrdiff_t>(c_str_len(set)), start);
    }

    ptrdiff_t find_last_not_of(const basic_string& set, ptrdiff_t start = npos) const noexcept {
        return find_last_not_of(set.ptr, set.len, start);
    }

    bool starts_with(const CharT* prefix, ptrdiff_t prefix_len) const noexcept {
        if (prefix_len < 0 || prefix_len > len) {
            return false;
        }
        if (prefix_len == 0) {
            return true;
        }
        return compare_chars(ptr, prefix, prefix_len) == 0;
    }

    bool starts_with(const CharT* prefix) const noexcept {
        if (prefix == nullptr) {
            return false;
        }
        return starts_with(prefix, static_cast<ptrdiff_t>(c_str_len(prefix)));
    }

    bool starts_with(CharT c) const noexcept {
        return len > 0 && ptr[0] == c;
    }

    bool ends_with(const CharT* suffix, ptrdiff_t suffix_len) const noexcept {
        if (suffix_len < 0 || suffix_len > len) {
            return false;
        }
        if (suffix_len == 0) {
            return true;
        }
        return compare_chars(ptr + (len - suffix_len), suffix, suffix_len) == 0;
    }

    bool ends_with(const CharT* suffix) const noexcept {
        if (suffix == nullptr) {
            return false;
        }
        return ends_with(suffix, static_cast<ptrdiff_t>(c_str_len(suffix)));
    }

    bool ends_with(CharT c) const noexcept {
        return len > 0 && ptr[len - 1] == c;
    }

    bool contains(const CharT* needle, ptrdiff_t needle_len) const noexcept {
        return find(needle, needle_len, 0) != npos;
    }

    bool contains(const CharT* needle) const noexcept {
        if (needle == nullptr) {
            return false;
        }
        return contains(needle, static_cast<ptrdiff_t>(c_str_len(needle)));
    }

    bool contains(CharT c) const noexcept {
        return find(c) != npos;
    }

    friend bool operator==(const basic_string& a, const basic_string& b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const basic_string& a, const basic_string& b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const basic_string& a, const basic_string& b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(const basic_string& a, const basic_string& b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(const basic_string& a, const basic_string& b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const basic_string& a, const basic_string& b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const basic_string& a, const CharT* b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const basic_string& a, const CharT* b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const basic_string& a, const CharT* b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(const basic_string& a, const CharT* b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(const basic_string& a, const CharT* b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const basic_string& a, const CharT* b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const CharT* a, const basic_string& b) noexcept { return b.compare(a) == 0; }
    friend bool operator!=(const CharT* a, const basic_string& b) noexcept { return b.compare(a) != 0; }
    friend bool operator<(const CharT* a, const basic_string& b) noexcept  { return b.compare(a) >  0; }
    friend bool operator>(const CharT* a, const basic_string& b) noexcept  { return b.compare(a) <  0; }
    friend bool operator<=(const CharT* a, const basic_string& b) noexcept { return b.compare(a) >= 0; }
    friend bool operator>=(const CharT* a, const basic_string& b) noexcept { return b.compare(a) <= 0; }

    void swap(basic_string& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(len, other.len);
        std::swap(cap, other.cap);
    }

    friend void swap(basic_string& a, basic_string& b) noexcept {
        a.swap(b);
    }

    friend basic_string operator+(const basic_string& a, const basic_string& b) {
        basic_string r;
        r.reserve(a.len + b.len);
        r.append(a);
        r.append(b);
        return r;
    }

    friend basic_string operator+(const basic_string& a, const CharT* b) {
        basic_string r = a;
        r.append(b);
        return r;
    }

    friend basic_string operator+(const CharT* a, const basic_string& b) {
        basic_string r;
        const ptrdiff_t an = a ? static_cast<ptrdiff_t>(c_str_len(a)) : 0;
        r.reserve(an + b.len);
        r.append(a ? a : empty_literal(), an);
        r.append(b);
        return r;
    }

private:
    CharT* ptr;
    ptrdiff_t len;
    ptrdiff_t cap;

    static const CharT* empty_literal() noexcept {
        static const CharT empty[1] = { CharT(0) };
        return empty;
    }

    static size_t c_str_len(const CharT* s) noexcept {
        if constexpr (std::is_same_v<CharT, char>) {
            return strlen(s);
        } else if constexpr (std::is_same_v<CharT, wchar_t>) {
            return wcslen(s);
        } else {
            size_t n = 0;
            while (s[n] != CharT(0)) {
                ++n;
            }
            return n;
        }
    }

    static int compare_chars(const CharT* a, const CharT* b, ptrdiff_t n) noexcept {
        if constexpr (sizeof(CharT) == 1) {
            return memcmp(a, b, static_cast<size_t>(n));
        } else {
            for (ptrdiff_t i = 0; i < n; ++i) {
                if (a[i] < b[i]) {
                    return -1;
                }
                if (b[i] < a[i]) {
                    return 1;
                }
            }
            return 0;
        }
    }

    static const void* mem_rchr(const void* buf, int c, size_t n) noexcept {
#if defined(__GLIBC__) || defined(__linux__) || defined(__APPLE__)
        return ::memrchr(buf, c, n);
#else
        const unsigned char* p = static_cast<const unsigned char*>(buf);
        const unsigned char target = static_cast<unsigned char>(c);
        for (size_t i = n; i > 0; --i) {
            if (p[i - 1] == target) {
                return p + (i - 1);
            }
        }
        return nullptr;
#endif
    }

    void ensure_null_slot() {
        if (cap == 0) {
            reallocate_to(min_cap);
        }
        ptr[len] = CharT(0);
    }

    static ptrdiff_t grow_to(ptrdiff_t needed) {
        ptrdiff_t new_cap = cap > 0 ? cap : min_cap;
        while (new_cap <= needed) {
            new_cap = new_cap + new_cap / 2;
        }
        return new_cap;
    }

    void reallocate_to(ptrdiff_t new_cap) {
        assert(new_cap > len);
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(CharT));
        assert(raw != nullptr && "basic_string allocation failed");
        CharT* fresh = static_cast<CharT*>(raw);
        if (len > 0) {
            memcpy(fresh, ptr, static_cast<size_t>(len) * sizeof(CharT));
        }
        fresh[len] = CharT(0);
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(CharT));
        }
        ptr = fresh;
        cap = new_cap;
    }
};

using string    = basic_string<char>;
using wstring   = basic_string<wchar_t>;
using u16string = basic_string<char16_t>;
using u32string = basic_string<char32_t>;

} // namespace df