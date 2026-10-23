#pragma once

// bc_string_builder.h
//
// Growable string builder for incremental construction. Wraps a
// basic_string<CharT> internally and adds numeric append + printf-style
// append_format. Uses basic_string's geometric growth and null-termination.
//
// Style: snake_case, One True Brace Style, template param CharT.

#include "bc_string.h"
#include "bc_string_view.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

namespace df {

template <typename CharT>
class basic_string_builder {
public:
    basic_string_builder() = default;

    explicit basic_string_builder(ptrdiff_t initial_capacity) {
        str_.reserve(initial_capacity);
    }

    basic_string_builder(const basic_string_builder&) = delete;
    basic_string_builder& operator=(const basic_string_builder&) = delete;

    basic_string_builder(basic_string_builder&&) noexcept = default;
    basic_string_builder& operator=(basic_string_builder&&) noexcept = default;

    ~basic_string_builder() = default;

    basic_string_builder& append(const CharT* s, ptrdiff_t n) {
        str_.append(s, n);
        return *this;
    }

    basic_string_builder& append(const CharT* s) {
        str_.append(s);
        return *this;
    }

    basic_string_builder& append(basic_string_view<CharT> v) {
        str_.append(v.ptr, v.len);
        return *this;
    }

    basic_string_builder& append(const basic_string<CharT>& s) {
        str_.append(s);
        return *this;
    }

    basic_string_builder& append_char(CharT c) {
        str_.push_back(c);
        return *this;
    }

    basic_string_builder& append_int(int64_t value) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        assert(n > 0 && static_cast<size_t>(n) < sizeof(buf));
        str_.append(buf, static_cast<ptrdiff_t>(n));
        return *this;
    }

    basic_string_builder& append_uint(uint64_t value) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
        assert(n > 0 && static_cast<size_t>(n) < sizeof(buf));
        str_.append(buf, static_cast<ptrdiff_t>(n));
        return *this;
    }

    basic_string_builder& append_float(float value, int precision = 6) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%.*f", precision, static_cast<double>(value));
        assert(n > 0 && static_cast<size_t>(n) < sizeof(buf));
        str_.append(buf, static_cast<ptrdiff_t>(n));
        return *this;
    }

    basic_string_builder& append_double(double value, int precision = 6) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%.*f", precision, value);
        assert(n > 0 && static_cast<size_t>(n) < sizeof(buf));
        str_.append(buf, static_cast<ptrdiff_t>(n));
        return *this;
    }

    basic_string_builder& append_hex(uint64_t value) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(value));
        assert(n > 0 && static_cast<size_t>(n) < sizeof(buf));
        str_.append(buf, static_cast<ptrdiff_t>(n));
        return *this;
    }

    basic_string_builder& append_format(const CharT* fmt, ...) {
        va_list args;
        va_start(args, fmt);

        va_list args_copy;
        va_copy(args_copy, args);
        const int needed = vsnprintf(nullptr, 0, fmt, args_copy);
        va_end(args_copy);

        if (needed > 0) {
            const ptrdiff_t old_len = str_.ssize();
            const ptrdiff_t new_len = old_len + static_cast<ptrdiff_t>(needed);
            str_.resize(new_len);
            vsnprintf(str_.data() + old_len,
                      static_cast<size_t>(needed + 1),
                      fmt, args);
        }
        va_end(args);
        return *this;
    }

    const CharT* c_str() const noexcept {
        return str_.c_str();
    }

    CharT* data() noexcept {
        return str_.data();
    }

    const CharT* data() const noexcept {
        return str_.data();
    }

    basic_string_view<CharT> view() const noexcept {
        return basic_string_view<CharT>(str_.data(), str_.ssize());
    }

    const basic_string<CharT>& str() const noexcept {
        return str_;
    }

    size_t size() const noexcept { return str_.size(); }
    ptrdiff_t ssize() const noexcept { return str_.ssize(); }
    bool empty() const noexcept { return str_.empty(); }
    size_t capacity() const noexcept { return str_.capacity(); }
    ptrdiff_t scapacity() const noexcept { return str_.scapacity(); }

    void clear() noexcept { str_.clear(); }
    void reserve(ptrdiff_t n) { str_.reserve(n); }

private:
    basic_string<CharT> str_;
};

using string_builder = basic_string_builder<char>;

} // namespace df