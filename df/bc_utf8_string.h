#pragma once

// bc_utf8_string.h
//
// Byte-storage container for a UTF-8 encoded string. The type guarantees
// the bytes form valid UTF-8 (asserted in debug; trusted in release).
//
// This is a STORAGE container, not a text-processing type. Its job is:
//   - own the bytes,
//   - keep them null-terminated,
//   - assert valid UTF-8 in debug,
//   - provide byte-level access plus code-point-boundary-aware editing.
//
// size() / ssize() return the BYTE count, not the code point count.
// For code-point counting, iteration, and encoding conversion, use
// bc_text_codec.h.
//
// Editing contract (insert / erase / replace / resize / substr):
//   - positions and lengths are BYTE offsets, not code point indices;
//   - pos must land on a UTF-8 code-point boundary;
//   - inserted bytes must be valid UTF-8 with no embedded null.
//
// resize(n)        — SHRINK ONLY. Asserts n <= len.
// resize(n, c)     — GROW ONLY. Asserts n >= len and c is ASCII non-null.
//
// Style: snake_case, One True Brace Style, bare size_t / ptrdiff_t / uintN_t.

#include "bc_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <utility>
#include <type_traits>

namespace df {

class utf8_string {
public:
    using value_type      = char;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = char&;
    using const_reference = const char&;
    using iterator        = char*;
    using const_iterator  = const char*;

    static constexpr ptrdiff_t min_cap = 16;
    static constexpr ptrdiff_t npos    = -1;

    utf8_string() noexcept : ptr(nullptr), len(0), cap(0) {
        ensure_null_slot();
    }

    utf8_string(const char* bytes, ptrdiff_t len_bytes) : ptr(nullptr), len(0), cap(0) {
        assert(len_bytes >= 0);
        if (len_bytes == 0) {
            ensure_null_slot();
            return;
        }
        reserve(len_bytes);
        memcpy(ptr, bytes, static_cast<size_t>(len_bytes));
        len = len_bytes;
        ptr[len] = '\0';
        assert_utf8();
    }

    utf8_string(const char* cstr) : ptr(nullptr), len(0), cap(0) {
        if (cstr == nullptr) {
            ensure_null_slot();
            return;
        }
        const ptrdiff_t n = static_cast<ptrdiff_t>(strlen(cstr));
        reserve(n);
        if (n > 0) {
            memcpy(ptr, cstr, static_cast<size_t>(n));
        }
        len = n;
        ptr[len] = '\0';
        assert_utf8();
    }

    utf8_string(const utf8_string& other) : ptr(nullptr), len(0), cap(0) {
        if (other.len > 0) {
            reserve(other.len);
            memcpy(ptr, other.ptr, static_cast<size_t>(other.len));
        }
        len = other.len;
        ptr[len] = '\0';
    }

    utf8_string(utf8_string&& other) noexcept : ptr(other.ptr), len(other.len), cap(other.cap) {
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
    }

    utf8_string& operator=(const utf8_string& other) {
        if (this == &other) {
            return *this;
        }
        utf8_string tmp(other);
        swap(tmp);
        return *this;
    }

    utf8_string& operator=(utf8_string&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap));
        }
        ptr = other.ptr;
        len = other.len;
        cap = other.cap;
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
        return *this;
    }

    utf8_string& operator=(const char* cstr) {
        assign(cstr);
        return *this;
    }

    ~utf8_string() {
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap));
        }
    }

    char& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }
    const char& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    char& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len && "utf8_string::at out of range");
        return ptr[i];
    }
    const char& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "utf8_string::at out of range");
        return ptr[i];
    }

    char& front() noexcept { assert(len > 0); return ptr[0]; }
    const char& front() const noexcept { assert(len > 0); return ptr[0]; }

    char& back() noexcept { assert(len > 0); return ptr[len - 1]; }
    const char& back() const noexcept { assert(len > 0); return ptr[len - 1]; }

    const char* c_str() const noexcept { return ptr ? ptr : empty_literal(); }
    char* data() noexcept { return ptr ? ptr : const_cast<char*>(empty_literal()); }
    const char* data() const noexcept { return ptr ? ptr : empty_literal(); }

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
        assert(new_len >= 0);
        assert(new_len <= len && "utf8_string::resize(n) shrinks only; use resize(n, c) to grow");
        len = new_len;
        ptr[len] = '\0';
        assert_utf8();
    }

    void resize(ptrdiff_t new_len, char c) {
        assert(new_len >= 0);
        assert(new_len >= len && "utf8_string::resize(n, c) grows only; use resize(n) to shrink");
        const unsigned char uc = static_cast<unsigned char>(c);
        assert(uc != 0x00 && "utf8_string::resize fill byte must be non-null");
        assert(uc < 0x80 && "utf8_string::resize fill byte must be ASCII");
        if (new_len == len) {
            return;
        }
        if (new_len + 1 > cap) {
            reserve(grow_to(new_len));
        }
        memset(ptr + len, c, static_cast<size_t>(new_len - len));
        len = new_len;
        ptr[len] = '\0';
        assert_utf8();
    }

    void clear() noexcept {
        len = 0;
        if (ptr != nullptr) {
            ptr[0] = '\0';
        }
    }

    void push_back(char c) {
        assert(static_cast<unsigned char>(c) < 0x80 && "utf8_string::push_back takes ASCII; use append for multi-byte");
        assert(c != '\0' && "utf8_string::push_back rejects embedded null");
        if (len + 1 >= cap) {
            reserve(grow_to(len + 1));
        }
        ptr[len] = c;
        ++len;
        ptr[len] = '\0';
        assert_utf8();
    }

    void pop_back() noexcept {
        assert(len > 0);
        --len;
        ptr[len] = '\0';
        assert_utf8();
    }

    void append(const char* bytes, ptrdiff_t len_bytes) {
        assert(len_bytes >= 0);
        if (len_bytes == 0) {
            return;
        }
        const ptrdiff_t need = len + len_bytes;
        if (need + 1 > cap) {
            reserve(grow_to(need));
        }
        memcpy(ptr + len, bytes, static_cast<size_t>(len_bytes));
        len = need;
        ptr[len] = '\0';
        assert_utf8();
    }

    void append(const char* cstr) {
        if (cstr == nullptr) {
            return;
        }
        append(cstr, static_cast<ptrdiff_t>(strlen(cstr)));
    }

    void append(const utf8_string& other) {
        append(other.ptr, other.len);
    }

    void assign(const char* bytes, ptrdiff_t len_bytes) {
        assert(len_bytes >= 0);
        clear();
        append(bytes, len_bytes);
    }

    void assign(const char* cstr) {
        clear();
        append(cstr);
    }

    void assign(const utf8_string& other) {
        if (this == &other) {
            return;
        }
        clear();
        append(other);
    }

    void insert(ptrdiff_t pos, const char* bytes, ptrdiff_t len_bytes) {
        assert(pos >= 0 && pos <= len);
        assert(len_bytes >= 0);
        assert(is_code_point_boundary(pos) && "utf8_string::insert pos not on code-point boundary");
        if (len_bytes == 0) {
            return;
        }
        assert(bytes != nullptr);
        assert_no_embedded_null(bytes, len_bytes);
        assert(is_valid_utf8(bytes, len_bytes) && "utf8_string::insert bytes are not valid UTF-8");

        const ptrdiff_t need = len + len_bytes;
        if (need + 1 > cap) {
            reserve(grow_to(need));
        }
        memmove(ptr + pos + len_bytes,
                ptr + pos,
                static_cast<size_t>(len - pos));
        memcpy(ptr + pos, bytes, static_cast<size_t>(len_bytes));
        len = need;
        ptr[len] = '\0';
        assert_utf8();
    }

    void insert(ptrdiff_t pos, const char* cstr) {
        if (cstr == nullptr) {
            return;
        }
        insert(pos, cstr, static_cast<ptrdiff_t>(strlen(cstr)));
    }

    void insert(ptrdiff_t pos, const utf8_string& other) {
        insert(pos, other.ptr, other.len);
    }

    void erase(ptrdiff_t pos, ptrdiff_t n) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0 && pos + n <= len);
        assert(is_code_point_boundary(pos) && "utf8_string::erase pos not on code-point boundary");
        assert(is_code_point_boundary(pos + n) && "utf8_string::erase end not on code-point boundary");
        if (n == 0) {
            return;
        }
        const ptrdiff_t tail = len - pos - n;
        if (tail > 0) {
            memmove(ptr + pos, ptr + pos + n, static_cast<size_t>(tail));
        }
        len -= n;
        ptr[len] = '\0';
        assert_utf8();
    }

    void erase(ptrdiff_t pos) {
        assert(pos >= 0 && pos < len);
        assert(is_code_point_boundary(pos) && "utf8_string::erase pos not on code-point boundary");
        erase(pos, code_point_len(ptr + pos));
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, const char* bytes, ptrdiff_t len_bytes) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0);
        assert(is_code_point_boundary(pos) && "utf8_string::replace pos not on code-point boundary");
        if (pos + n > len) {
            n = len - pos;
        }
        assert(is_code_point_boundary(pos + n) && "utf8_string::replace end not on code-point boundary");
        if (len_bytes == 0) {
            erase(pos, n);
            return;
        }
        assert(bytes != nullptr);
        assert_no_embedded_null(bytes, len_bytes);
        assert(is_valid_utf8(bytes, len_bytes) && "utf8_string::replace bytes are not valid UTF-8");

        const ptrdiff_t removed_tail = len - pos - n;
        const ptrdiff_t new_len = pos + len_bytes + removed_tail;
        if (new_len + 1 > cap) {
            reserve(grow_to(new_len));
        }
        if (removed_tail > 0 && n != len_bytes) {
            memmove(ptr + pos + len_bytes, ptr + pos + n,
                    static_cast<size_t>(removed_tail));
        }
        if (len_bytes > 0) {
            memmove(ptr + pos, bytes, static_cast<size_t>(len_bytes));
        }
        len = new_len;
        ptr[len] = '\0';
        assert_utf8();
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, const char* cstr) {
        if (cstr == nullptr) {
            replace(pos, n, nullptr, 0);
            return;
        }
        replace(pos, n, cstr, static_cast<ptrdiff_t>(strlen(cstr)));
    }

    void replace(ptrdiff_t pos, ptrdiff_t n, const utf8_string& other) {
        replace(pos, n, other.ptr, other.len);
    }

    utf8_string substr(ptrdiff_t pos, ptrdiff_t n = npos) const {
        assert(pos >= 0 && pos <= len);
        assert(is_code_point_boundary(pos) && "utf8_string::substr pos not on code-point boundary");
        ptrdiff_t count = (n == npos || pos + n > len) ? (len - pos) : n;
        if (count < 0) {
            count = 0;
        }
        assert(is_code_point_boundary(pos + count) && "utf8_string::substr end not on code-point boundary");
        return utf8_string(ptr + pos, count);
    }

    ptrdiff_t find(char c, ptrdiff_t start = 0) const noexcept {
        if (start < 0 || start > len) {
            return npos;
        }
        const void* hit = memchr(ptr + start, static_cast<unsigned char>(c),
                                 static_cast<size_t>(len - start));
        if (hit == nullptr) {
            return npos;
        }
        return static_cast<ptrdiff_t>(static_cast<const char*>(hit) - ptr);
    }

    ptrdiff_t find(const char* needle, ptrdiff_t start = 0) const noexcept {
        if (needle == nullptr) {
            return npos;
        }
        return find(needle, static_cast<ptrdiff_t>(strlen(needle)), start);
    }

    ptrdiff_t find(const char* needle, ptrdiff_t needle_len, ptrdiff_t start) const noexcept {
        if (needle == nullptr || needle_len <= 0) {
            return start <= len ? start : npos;
        }
        if (start < 0 || start > len) {
            return npos;
        }
        if (needle_len > len - start) {
            return npos;
        }
        const char first = needle[0];
        const ptrdiff_t last_possible = len - needle_len;
        ptrdiff_t cur = start;
        while (cur <= last_possible) {
            const void* hit = memchr(ptr + cur, static_cast<unsigned char>(first),
                                     static_cast<size_t>(last_possible - cur + 1));
            if (hit == nullptr) {
                return npos;
            }
            const char* h = static_cast<const char*>(hit);
            if (memcmp(h, needle, static_cast<size_t>(needle_len)) == 0) {
                return static_cast<ptrdiff_t>(h - ptr);
            }
            cur = static_cast<ptrdiff_t>(h - ptr) + 1;
        }
        return npos;
    }

    ptrdiff_t find(const utf8_string& needle, ptrdiff_t start = 0) const noexcept {
        return find(needle.ptr, needle.len, start);
    }

    bool starts_with(const char* prefix, ptrdiff_t prefix_len) const noexcept {
        if (prefix_len < 0 || prefix_len > len) {
            return false;
        }
        if (prefix_len == 0) {
            return true;
        }
        return memcmp(ptr, prefix, static_cast<size_t>(prefix_len)) == 0;
    }

    bool starts_with(const char* prefix) const noexcept {
        if (prefix == nullptr) {
            return false;
        }
        return starts_with(prefix, static_cast<ptrdiff_t>(strlen(prefix)));
    }

    bool starts_with(char c) const noexcept {
        return len > 0 && ptr[0] == c;
    }

    bool ends_with(const char* suffix, ptrdiff_t suffix_len) const noexcept {
        if (suffix_len < 0 || suffix_len > len) {
            return false;
        }
        if (suffix_len == 0) {
            return true;
        }
        return memcmp(ptr + (len - suffix_len), suffix, static_cast<size_t>(suffix_len)) == 0;
    }

    bool ends_with(const char* suffix) const noexcept {
        if (suffix == nullptr) {
            return false;
        }
        return ends_with(suffix, static_cast<ptrdiff_t>(strlen(suffix)));
    }

    bool ends_with(char c) const noexcept {
        return len > 0 && ptr[len - 1] == c;
    }

    bool contains(const char* needle, ptrdiff_t needle_len) const noexcept {
        return find(needle, needle_len, 0) != npos;
    }

    bool contains(const char* needle) const noexcept {
        if (needle == nullptr) {
            return false;
        }
        return contains(needle, static_cast<ptrdiff_t>(strlen(needle)));
    }

    bool contains(char c) const noexcept {
        return find(c) != npos;
    }

    int compare(const utf8_string& other) const noexcept {
        const ptrdiff_t n = (len < other.len) ? len : other.len;
        if (n > 0) {
            const int r = memcmp(ptr, other.ptr, static_cast<size_t>(n));
            if (r != 0) {
                return r;
            }
        }
        if (len < other.len) return -1;
        if (len > other.len) return 1;
        return 0;
    }

    int compare(const char* cstr) const noexcept {
        if (cstr == nullptr) {
            return len == 0 ? 0 : 1;
        }
        const ptrdiff_t other_len = static_cast<ptrdiff_t>(strlen(cstr));
        const ptrdiff_t n = (len < other_len) ? len : other_len;
        if (n > 0) {
            const int r = memcmp(ptr, cstr, static_cast<size_t>(n));
            if (r != 0) {
                return r;
            }
        }
        if (len < other_len) return -1;
        if (len > other_len) return 1;
        return 0;
    }

    friend bool operator==(const utf8_string& a, const utf8_string& b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const utf8_string& a, const utf8_string& b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const utf8_string& a, const utf8_string& b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(const utf8_string& a, const utf8_string& b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(const utf8_string& a, const utf8_string& b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const utf8_string& a, const utf8_string& b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const utf8_string& a, const char* b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const utf8_string& a, const char* b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const utf8_string& a, const char* b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(const utf8_string& a, const char* b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(const utf8_string& a, const char* b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const utf8_string& a, const char* b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const char* a, const utf8_string& b) noexcept { return b.compare(a) == 0; }
    friend bool operator!=(const char* a, const utf8_string& b) noexcept { return b.compare(a) != 0; }
    friend bool operator<(const char* a, const utf8_string& b) noexcept  { return b.compare(a) >  0; }
    friend bool operator>(const char* a, const utf8_string& b) noexcept  { return b.compare(a) <  0; }
    friend bool operator<=(const char* a, const utf8_string& b) noexcept { return b.compare(a) >= 0; }
    friend bool operator>=(const char* a, const utf8_string& b) noexcept { return b.compare(a) <= 0; }

    void swap(utf8_string& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(len, other.len);
        std::swap(cap, other.cap);
    }

    friend void swap(utf8_string& a, utf8_string& b) noexcept {
        a.swap(b);
    }

private:
    char* ptr;
    ptrdiff_t len;
    ptrdiff_t cap;

    static const char* empty_literal() noexcept {
        static const char empty[1] = { '\0' };
        return empty;
    }

    void ensure_null_slot() {
        if (cap == 0) {
            reallocate_to(min_cap);
        }
        ptr[len] = '\0';
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
        void* raw = df::allocate(static_cast<size_t>(new_cap));
        assert(raw != nullptr && "utf8_string: allocation failed");
        char* fresh = static_cast<char*>(raw);
        if (len > 0) {
            memcpy(fresh, ptr, static_cast<size_t>(len));
        }
        fresh[len] = '\0';
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap));
        }
        ptr = fresh;
        cap = new_cap;
    }

    static ptrdiff_t code_point_len(const char* p) noexcept {
        const unsigned char b = static_cast<unsigned char>(*p);
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        assert(false && "utf8_string: code_point_len on invalid lead byte");
        return 1;
    }

    bool is_code_point_boundary(ptrdiff_t pos) const noexcept {
        if (pos == 0 || pos == len) {
            return true;
        }
        if (pos < 0 || pos > len) {
            return false;
        }
        const unsigned char b = static_cast<unsigned char>(ptr[pos]);
        return (b & 0xC0) != 0x80;
    }

    static bool is_valid_utf8(const char* bytes, ptrdiff_t n) noexcept {
        ptrdiff_t i = 0;
        while (i < n) {
            const unsigned char b = static_cast<unsigned char>(bytes[i]);
            ptrdiff_t cont = 0;
            if (b < 0x80) {
                cont = 0;
            } else if ((b & 0xE0) == 0xC0) {
                cont = 1;
            } else if ((b & 0xF0) == 0xE0) {
                cont = 2;
            } else if ((b & 0xF8) == 0xF0) {
                cont = 3;
            } else {
                return false;
            }
            if (i + cont >= n) {
                return false;
            }
            for (ptrdiff_t c = 1; c <= cont; ++c) {
                if ((static_cast<unsigned char>(bytes[i + c]) & 0xC0) != 0x80) {
                    return false;
                }
            }
            i += cont + 1;
        }
        return true;
    }

    static void assert_no_embedded_null(const char* bytes, ptrdiff_t n) noexcept {
#if !defined(NDEBUG)
        assert(memchr(bytes, '\0', static_cast<size_t>(n)) == nullptr &&
               "utf8_string: embedded null in input bytes");
#else
        (void)bytes; (void)n;
#endif
    }

    void assert_utf8() const noexcept {
#if !defined(NDEBUG)
        ptrdiff_t i = 0;
        while (i < len) {
            const unsigned char b = static_cast<unsigned char>(ptr[i]);
            if (b == 0x00) {
                assert(false && "utf8_string: embedded null in byte range");
                return;
            }
            ptrdiff_t continuation = 0;
            if (b < 0x80) {
                continuation = 0;
            } else if ((b & 0xE0) == 0xC0) {
                continuation = 1;
            } else if ((b & 0xF0) == 0xE0) {
                continuation = 2;
            } else if ((b & 0xF8) == 0xF0) {
                continuation = 3;
            } else {
                assert(false && "utf8_string: invalid UTF-8 lead byte");
                return;
            }
            if (i + continuation >= len) {
                assert(false && "utf8_string: truncated UTF-8 sequence");
                return;
            }
            for (ptrdiff_t c = 1; c <= continuation; ++c) {
                const unsigned char cb = static_cast<unsigned char>(ptr[i + c]);
                if ((cb & 0xC0) != 0x80) {
                    assert(false && "utf8_string: invalid UTF-8 continuation byte");
                    return;
                }
            }
            i += continuation + 1;
        }
#endif
    }
};

} // namespace df