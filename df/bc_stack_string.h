#pragma once

// bc_stack_string.h
//
// Fixed-capacity string with the storage inline on the stack, templated on
// character type and buffer size. Intended for character types only.
//
// Model: T ptr[N] = {}; int len = 0; static constexpr int bcap = N;
//
// Style: snake_case, One True Brace Style, template params T, N.

#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#include <type_traits>

namespace df {

template <typename T, int N>
class stack_basic_string {
    static_assert(N >= 1, "stack_basic_string needs at least one slot for the null terminator");

public:
    T ptr[N] = {};
    int len = 0;
    static constexpr int bcap = N;

    stack_basic_string() = default;

    stack_basic_string(const T* s) {
        assign(s);
    }

    size_t size() const noexcept { return static_cast<size_t>(len); }
    int ssize() const noexcept { return len; }
    int capacity() const noexcept { return bcap - 1; }
    bool empty() const noexcept { return len == 0; }
    bool full() const noexcept { return len >= bcap - 1; }

    T* data() noexcept { return ptr; }
    const T* data() const noexcept { return ptr; }
    const T* c_str() const noexcept { return ptr; }

    T& operator[](int i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }
    const T& operator[](int i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    T& at(int i) noexcept {
        assert(i >= 0 && i < len && "stack_basic_string::at out of range");
        return ptr[i];
    }
    const T& at(int i) const noexcept {
        assert(i >= 0 && i < len && "stack_basic_string::at out of range");
        return ptr[i];
    }

    T& front() noexcept { assert(len > 0); return ptr[0]; }
    const T& front() const noexcept { assert(len > 0); return ptr[0]; }

    T& back() noexcept { assert(len > 0); return ptr[len - 1]; }
    const T& back() const noexcept { assert(len > 0); return ptr[len - 1]; }

    T* begin() noexcept { return ptr; }
    T* end()   noexcept { return ptr + len; }
    const T* begin() const noexcept { return ptr; }
    const T* end()   const noexcept { return ptr + len; }
    const T* cbegin() const noexcept { return ptr; }
    const T* cend()   const noexcept { return ptr + len; }

    bool push_back(T c) {
        if (len >= bcap - 1) {
            assert(false && "stack_basic_string::push_back on full");
            return false;
        }
        ptr[len] = c;
        ++len;
        ptr[len] = T(0);
        return true;
    }

    void pop_back() {
        if (len <= 0) {
            assert(false && "stack_basic_string::pop_back on empty");
            return;
        }
        --len;
        ptr[len] = T(0);
    }

    void clear() noexcept {
        len = 0;
        ptr[0] = T(0);
    }

    bool append(const T* s) {
        if (s == nullptr) {
            return true;
        }
        const int n = static_cast<int>(c_str_len(s));
        return append(s, n);
    }

    bool append(const T* s, int n) {
        assert(s != nullptr);
        assert(n >= 0);
        if (len + n > bcap - 1) {
            assert(false && "stack_basic_string::append overflow");
            return false;
        }
        memcpy(ptr + len, s, static_cast<size_t>(n) * sizeof(T));
        len += n;
        ptr[len] = T(0);
        return true;
    }

    bool append(const stack_basic_string& other) {
        return append(other.ptr, other.len);
    }

    void assign(const T* s) {
        if (s == nullptr) {
            clear();
            return;
        }
        assign(s, static_cast<int>(c_str_len(s)));
    }

    void assign(const T* s, int n) {
        assert(s != nullptr);
        assert(n >= 0);
        len = 0;
        ptr[0] = T(0);
        if (n > bcap - 1) {
            assert(false && "stack_basic_string::assign overflow");
            return;
        }
        memcpy(ptr, s, static_cast<size_t>(n) * sizeof(T));
        len = n;
        ptr[len] = T(0);
    }

    void assign(const stack_basic_string& other) {
        assign(other.ptr, other.len);
    }

    void resize(int new_len) {
        resize(new_len, T(0));
    }

    void resize(int new_len, T fill) {
        if (new_len < 0) {
            assert(false && "stack_basic_string::resize negative len");
            new_len = 0;
        }
        if (new_len > bcap - 1) {
            assert(false && "stack_basic_string::resize past capacity");
            new_len = bcap - 1;
        }
        if (new_len > len) {
            for (int i = len; i < new_len; ++i) {
                ptr[i] = fill;
            }
        }
        len = new_len;
        ptr[len] = T(0);
    }

    bool insert(int pos, const T* s, int n) {
        assert(pos >= 0 && pos <= len);
        assert(s != nullptr || n == 0);
        assert(n >= 0);
        if (n == 0) {
            return true;
        }
        if (len + n > bcap - 1) {
            assert(false && "stack_basic_string::insert overflow");
            return false;
        }
        memmove(ptr + pos + n, ptr + pos,
                static_cast<size_t>(len - pos) * sizeof(T));
        memcpy(ptr + pos, s, static_cast<size_t>(n) * sizeof(T));
        len += n;
        ptr[len] = T(0);
        return true;
    }

    bool insert(int pos, T c) {
        return insert(pos, &c, 1);
    }

    void erase(int pos, int n) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0 && pos + n <= len);
        if (n == 0) {
            return;
        }
        const int tail = len - pos - n;
        if (tail > 0) {
            memmove(ptr + pos, ptr + pos + n,
                    static_cast<size_t>(tail) * sizeof(T));
        }
        len -= n;
        ptr[len] = T(0);
    }

    void erase(int pos) { erase(pos, 1); }

    bool replace(int pos, int n, const T* s, int m) {
        assert(pos >= 0 && pos <= len);
        assert(n >= 0);
        assert(s != nullptr || m == 0);
        assert(m >= 0);
        if (pos + n > len) {
            n = len - pos;
        }
        const int removed_tail = len - pos - n;
        const int new_len = pos + m + removed_tail;
        if (new_len > bcap - 1) {
            assert(false && "stack_basic_string::replace overflow");
            return false;
        }
        if (removed_tail > 0 && n != m) {
            memmove(ptr + pos + m, ptr + pos + n,
                    static_cast<size_t>(removed_tail) * sizeof(T));
        }
        if (m > 0) {
            memmove(ptr + pos, s, static_cast<size_t>(m) * sizeof(T));
        }
        len = new_len;
        ptr[len] = T(0);
        return true;
    }

    bool replace(int pos, int n, const T* s) {
        if (s == nullptr) {
            return replace(pos, n, nullptr, 0);
        }
        return replace(pos, n, s, static_cast<int>(c_str_len(s)));
    }

    bool replace(int pos, int n, T c) {
        return replace(pos, n, &c, 1);
    }

    bool replace(int pos, int n, const stack_basic_string& other) {
        return replace(pos, n, other.ptr, other.len);
    }

    int find(T c, int start = 0) const {
        if (start < 0 || start > len) {
            return -1;
        }
        for (int i = start; i < len; ++i) {
            if (ptr[i] == c) {
                return i;
            }
        }
        return -1;
    }

    int find(const T* needle, int start = 0) const {
        if (needle == nullptr) {
            return -1;
        }
        return find(needle, static_cast<int>(c_str_len(needle)), start);
    }

    int find(const T* needle, int needle_len, int start) const {
        if (needle == nullptr || needle_len < 0) {
            return -1;
        }
        if (needle_len == 0) {
            return (start >= 0 && start <= len) ? start : -1;
        }
        if (start < 0 || start > len - needle_len) {
            return -1;
        }
        const int last_start = len - needle_len;
        for (int i = start; i <= last_start; ++i) {
            if (compare_chars(ptr + i, needle, needle_len) == 0) {
                return i;
            }
        }
        return -1;
    }

    int find(const stack_basic_string& needle, int start = 0) const {
        return find(needle.ptr, needle.len, start);
    }

    bool starts_with(const T* prefix, int prefix_len) const {
        if (prefix_len < 0 || prefix_len > len) {
            return false;
        }
        if (prefix_len == 0) {
            return true;
        }
        return compare_chars(ptr, prefix, prefix_len) == 0;
    }

    bool starts_with(const T* prefix) const {
        if (prefix == nullptr) {
            return false;
        }
        return starts_with(prefix, static_cast<int>(c_str_len(prefix)));
    }

    bool starts_with(T c) const {
        return len > 0 && ptr[0] == c;
    }

    bool ends_with(const T* suffix, int suffix_len) const {
        if (suffix_len < 0 || suffix_len > len) {
            return false;
        }
        if (suffix_len == 0) {
            return true;
        }
        return compare_chars(ptr + (len - suffix_len), suffix, suffix_len) == 0;
    }

    bool ends_with(const T* suffix) const {
        if (suffix == nullptr) {
            return false;
        }
        return ends_with(suffix, static_cast<int>(c_str_len(suffix)));
    }

    bool ends_with(T c) const {
        return len > 0 && ptr[len - 1] == c;
    }

    bool contains(const T* needle, int needle_len) const {
        return find(needle, needle_len, 0) >= 0;
    }

    bool contains(const T* needle) const {
        if (needle == nullptr) {
            return false;
        }
        return contains(needle, static_cast<int>(c_str_len(needle)));
    }

    bool contains(T c) const {
        return find(c) >= 0;
    }

    bool contains(const stack_basic_string& needle) const {
        return find(needle.ptr, needle.len, 0) >= 0;
    }

    int compare(const stack_basic_string& other) const noexcept {
        const int n = (len < other.len) ? len : other.len;
        if (n > 0) {
            const int r = compare_chars(ptr, other.ptr, n);
            if (r != 0) {
                return r;
            }
        }
        if (len < other.len) {
            return -1;
        }
        if (len > other.len) {
            return 1;
        }
        return 0;
    }

    int compare(const T* s) const noexcept {
        if (s == nullptr) {
            return (len == 0) ? 0 : 1;
        }
        const int other_len = static_cast<int>(c_str_len(s));
        const int n = (len < other_len) ? len : other_len;
        if (n > 0) {
            const int r = compare_chars(ptr, s, n);
            if (r != 0) {
                return r;
            }
        }
        if (len < other_len) {
            return -1;
        }
        if (len > other_len) {
            return 1;
        }
        return 0;
    }

    stack_basic_string substr(int pos, int n = -1) const {
        if (pos < 0 || pos > len) {
            return stack_basic_string();
        }
        if (n < 0 || pos + n > len) {
            n = len - pos;
        }
        stack_basic_string result;
        result.assign(ptr + pos, n);
        return result;
    }

    void swap(stack_basic_string& other) noexcept {
        const int shorter = (len < other.len) ? len : other.len;
        for (int i = 0; i < shorter; ++i) {
            T tmp = ptr[i];
            ptr[i] = other.ptr[i];
            other.ptr[i] = tmp;
        }
        if (len > other.len) {
            for (int i = shorter; i < len; ++i) {
                other.ptr[i] = ptr[i];
            }
        } else if (other.len > len) {
            for (int i = shorter; i < other.len; ++i) {
                ptr[i] = other.ptr[i];
            }
        }
        for (int i = len; i < bcap; ++i) {
            other.ptr[i] = T(0);
        }
        for (int i = other.len; i < bcap; ++i) {
            ptr[i] = T(0);
        }
        const int tmp_len = len;
        len = other.len;
        other.len = tmp_len;
        ptr[len] = T(0);
        other.ptr[other.len] = T(0);
    }

    friend void swap(stack_basic_string& a, stack_basic_string& b) noexcept {
        a.swap(b);
    }

    friend bool operator==(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) == 0;
    }
    friend bool operator!=(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) != 0;
    }
    friend bool operator<(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) < 0;
    }
    friend bool operator>(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) > 0;
    }
    friend bool operator<=(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) <= 0;
    }
    friend bool operator>=(const stack_basic_string& a, const stack_basic_string& b) noexcept {
        return a.compare(b) >= 0;
    }

    friend bool operator==(const stack_basic_string& a, const T* b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const stack_basic_string& a, const T* b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const stack_basic_string& a, const T* b) noexcept  { return a.compare(b) <  0; }
    friend bool operator>(const stack_basic_string& a, const T* b) noexcept  { return a.compare(b) >  0; }
    friend bool operator<=(const stack_basic_string& a, const T* b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const stack_basic_string& a, const T* b) noexcept { return a.compare(b) >= 0; }

    friend bool operator==(const T* a, const stack_basic_string& b) noexcept { return b.compare(a) == 0; }
    friend bool operator!=(const T* a, const stack_basic_string& b) noexcept { return b.compare(a) != 0; }
    friend bool operator<(const T* a, const stack_basic_string& b) noexcept  { return b.compare(a) >  0; }
    friend bool operator>(const T* a, const stack_basic_string& b) noexcept  { return b.compare(a) <  0; }
    friend bool operator<=(const T* a, const stack_basic_string& b) noexcept { return b.compare(a) >= 0; }
    friend bool operator>=(const T* a, const stack_basic_string& b) noexcept { return b.compare(a) <= 0; }

private:
    static size_t c_str_len(const T* s) noexcept {
        if constexpr (std::is_same_v<T, char>) {
            return strlen(s);
        } else if constexpr (std::is_same_v<T, wchar_t>) {
            return wcslen(s);
        } else {
            size_t n = 0;
            while (s[n] != T(0)) {
                ++n;
            }
            return n;
        }
    }

    static int compare_chars(const T* a, const T* b, int n) noexcept {
        if constexpr (sizeof(T) == 1) {
            return memcmp(a, b, static_cast<size_t>(n));
        } else {
            for (int i = 0; i < n; ++i) {
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
};

template <int N> using stack_string    = stack_basic_string<char, N>;
template <int N> using stack_wstring   = stack_basic_string<wchar_t, N>;
template <int N> using stack_u16string = stack_basic_string<char16_t, N>;
template <int N> using stack_u32string = stack_basic_string<char32_t, N>;

} // namespace df