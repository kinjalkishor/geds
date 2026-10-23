#pragma once

// bc_string_view.h
//
// Non-owning view over a null-terminated-or-arbitrary-length sequence of
// CharT. Has find, rfind, substr, compare, starts_with, ends_with, contains.
//
// Style: snake_case, One True Brace Style, template param CharT.

#include "bc_string.h"

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <type_traits>

namespace df {

template <typename CharT>
class basic_string_view {
public:
    using value_type      = CharT;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = const CharT&;
    using const_reference = const CharT&;
    using pointer         = const CharT*;
    using const_pointer   = const CharT*;
    using iterator        = const CharT*;
    using const_iterator  = const CharT*;

    static constexpr ptrdiff_t npos = -1;

    const CharT* ptr;
    ptrdiff_t    len;

    constexpr basic_string_view() noexcept
        : ptr(nullptr), len(0) {}

    constexpr basic_string_view(const CharT* p, ptrdiff_t n) noexcept
        : ptr(p), len(n) {}

    basic_string_view(const CharT* cstr) noexcept
        : ptr(cstr), len(static_cast<ptrdiff_t>(c_str_len(cstr))) {}

    basic_string_view(const df::basic_string<CharT>& s) noexcept
        : ptr(s.data()), len(s.ssize()) {}

    template <typename Container,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<Container>, basic_string_view>>>
    basic_string_view(const Container& c) noexcept
        : ptr(c.data()), len(static_cast<ptrdiff_t>(c.size())) {}

    size_t size() const noexcept { return static_cast<size_t>(len); }
    ptrdiff_t ssize() const noexcept { return len; }
    bool empty() const noexcept { return len == 0; }

    const CharT* data() const noexcept { return ptr; }
    const CharT* begin() const noexcept { return ptr; }
    const CharT* end() const noexcept { return ptr + len; }
    const CharT* cbegin() const noexcept { return ptr; }
    const CharT* cend() const noexcept { return ptr + len; }

    const CharT& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    const CharT& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "basic_string_view::at out of range");
        return ptr[i];
    }

    const CharT& front() const noexcept { assert(len > 0); return ptr[0]; }
    const CharT& back() const noexcept { assert(len > 0); return ptr[len - 1]; }

    basic_string_view substr(ptrdiff_t pos, ptrdiff_t n = npos) const noexcept {
        assert(pos >= 0 && pos <= len);
        ptrdiff_t count = (n == npos || pos + n > len) ? (len - pos) : n;
        if (count < 0) {
            count = 0;
        }
        return basic_string_view(ptr + pos, count);
    }

    basic_string_view first(ptrdiff_t n) const noexcept {
        assert(n >= 0 && n <= len);
        return basic_string_view(ptr, n);
    }

    basic_string_view last(ptrdiff_t n) const noexcept {
        assert(n >= 0 && n <= len);
        return basic_string_view(ptr + (len - n), n);
    }

    void remove_prefix(ptrdiff_t n) noexcept {
        assert(n >= 0 && n <= len);
        ptr += n;
        len -= n;
    }

    void remove_suffix(ptrdiff_t n) noexcept {
        assert(n >= 0 && n <= len);
        len -= n;
    }

    ptrdiff_t find(CharT c, ptrdiff_t start = 0) const noexcept {
        if (start < 0 || start > len) {
            return npos;
        }
        if constexpr (sizeof(CharT) == 1) {
            const void* hit = memchr(ptr + start, static_cast<unsigned char>(c),
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

    ptrdiff_t find(const CharT* needle, ptrdiff_t needle_len, ptrdiff_t start) const noexcept {
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
                const void* hit = memchr(ptr + cur, static_cast<unsigned char>(first),
                                         static_cast<size_t>(last_possible - cur + 1));
                if (hit == nullptr) {
                    return npos;
                }
                const CharT* h = static_cast<const CharT*>(hit);
                if (memcmp(h, needle, static_cast<size_t>(needle_len) * sizeof(CharT)) == 0) {
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

    ptrdiff_t find(basic_string_view needle, ptrdiff_t start = 0) const noexcept {
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

    ptrdiff_t rfind(const CharT* needle, ptrdiff_t needle_len, ptrdiff_t start = npos) const noexcept {
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
                if (memcmp(h, needle, static_cast<size_t>(needle_len) * sizeof(CharT)) == 0) {
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

    ptrdiff_t rfind(basic_string_view needle, ptrdiff_t start = npos) const noexcept {
        return rfind(needle.ptr, needle.len, start);
    }

    ptrdiff_t find_first_of(const CharT* set, ptrdiff_t set_len, ptrdiff_t start = 0) const noexcept {
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

    ptrdiff_t find_first_of(basic_string_view set, ptrdiff_t start = 0) const noexcept {
        return find_first_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_last_of(const CharT* set, ptrdiff_t set_len, ptrdiff_t start = npos) const noexcept {
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

    ptrdiff_t find_last_of(basic_string_view set, ptrdiff_t start = npos) const noexcept {
        return find_last_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_first_not_of(const CharT* set, ptrdiff_t set_len, ptrdiff_t start = 0) const noexcept {
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

    ptrdiff_t find_first_not_of(basic_string_view set, ptrdiff_t start = 0) const noexcept {
        return find_first_not_of(set.ptr, set.len, start);
    }

    ptrdiff_t find_last_not_of(const CharT* set, ptrdiff_t set_len, ptrdiff_t start = npos) const noexcept {
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

    ptrdiff_t find_last_not_of(basic_string_view set, ptrdiff_t start = npos) const noexcept {
        return find_last_not_of(set.ptr, set.len, start);
    }

    int compare(basic_string_view other) const noexcept {
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
            return (len == 0) ? 0 : 1;
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

    bool starts_with(basic_string_view prefix) const noexcept {
        if (prefix.len > len) {
            return false;
        }
        return compare_chars(ptr, prefix.ptr, prefix.len) == 0;
    }

    bool starts_with(CharT c) const noexcept {
        return len > 0 && ptr[0] == c;
    }

    bool starts_with(const CharT* cstr) const noexcept {
        if (cstr == nullptr) {
            return false;
        }
        const ptrdiff_t n = static_cast<ptrdiff_t>(c_str_len(cstr));
        if (n > len) {
            return false;
        }
        return compare_chars(ptr, cstr, n) == 0;
    }

    bool ends_with(basic_string_view suffix) const noexcept {
        if (suffix.len > len) {
            return false;
        }
        return compare_chars(ptr + (len - suffix.len), suffix.ptr, suffix.len) == 0;
    }

    bool ends_with(CharT c) const noexcept {
        return len > 0 && ptr[len - 1] == c;
    }

    bool ends_with(const CharT* cstr) const noexcept {
        if (cstr == nullptr) {
            return false;
        }
        const ptrdiff_t n = static_cast<ptrdiff_t>(c_str_len(cstr));
        if (n > len) {
            return false;
        }
        return compare_chars(ptr + (len - n), cstr, n) == 0;
    }

    bool contains(basic_string_view needle) const noexcept {
        return find(needle) != npos;
    }

    bool contains(CharT c) const noexcept {
        return find(c) != npos;
    }

    bool contains(const CharT* cstr) const noexcept {
        if (cstr == nullptr) {
            return false;
        }
        return find(cstr) != npos;
    }

    friend bool operator==(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) == 0;
    }
    friend bool operator!=(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) != 0;
    }
    friend bool operator<(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) < 0;
    }
    friend bool operator>(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) > 0;
    }
    friend bool operator<=(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) <= 0;
    }
    friend bool operator>=(basic_string_view a, basic_string_view b) noexcept {
        return a.compare(b) >= 0;
    }

    friend bool operator==(basic_string_view a, const CharT* b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(basic_string_view a, const CharT* b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(basic_string_view a, const CharT* b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(basic_string_view a, const CharT* b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(basic_string_view a, const CharT* b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(basic_string_view a, const CharT* b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const CharT* a, basic_string_view b) noexcept { return b.compare(a) == 0; }
    friend bool operator!=(const CharT* a, basic_string_view b) noexcept { return b.compare(a) != 0; }
    friend bool operator<(const CharT* a, basic_string_view b) noexcept  { return b.compare(a) >  0; }
    friend bool operator>(const CharT* a, basic_string_view b) noexcept  { return b.compare(a) <  0; }
    friend bool operator<=(const CharT* a, basic_string_view b) noexcept { return b.compare(a) >= 0; }
    friend bool operator>=(const CharT* a, basic_string_view b) noexcept { return b.compare(a) <= 0; }

private:
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
};

using string_view    = basic_string_view<char>;
using wstring_view   = basic_string_view<wchar_t>;
using u16string_view = basic_string_view<char16_t>;
using u32string_view = basic_string_view<char32_t>;

} // namespace df