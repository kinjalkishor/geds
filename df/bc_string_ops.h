#pragma once

// bc_string_ops.h
//
// Non-allocating string-view operations on basic_string_view<CharT> and
// span<const CharT>: trim, split, join.
//
// trim accepts either:
//   - a single character to strip (trim(s, ' ')),
//   - a set of characters to strip (trim(s, " \t\r\n"sv)), or
//   - no argument (defaults to stripping spaces).
//
// Style: snake_case, One True Brace Style, template params CharT / Fn.

#include "bc_string_view.h"
#include "bc_string.h"
#include "bc_string_builder.h"
#include "bc_span.h"
#include "bc_vector.h"

#include <stddef.h>
#include <stdint.h>

namespace df {

template <typename CharT>
basic_string_view<CharT> trim_left(basic_string_view<CharT> s, CharT c) noexcept {
    ptrdiff_t i = 0;
    while (i < s.len && s.ptr[i] == c) {
        ++i;
    }
    return basic_string_view<CharT>(s.ptr + i, s.len - i);
}

template <typename CharT>
basic_string_view<CharT> trim_right(basic_string_view<CharT> s, CharT c) noexcept {
    ptrdiff_t n = s.len;
    while (n > 0 && s.ptr[n - 1] == c) {
        --n;
    }
    return basic_string_view<CharT>(s.ptr, n);
}

template <typename CharT>
basic_string_view<CharT> trim(basic_string_view<CharT> s, CharT c) noexcept {
    return trim_right(trim_left(s, c), c);
}

template <typename CharT>
basic_string_view<CharT> trim_left(basic_string_view<CharT> s,
                                   basic_string_view<CharT> chars) noexcept {
    ptrdiff_t i = 0;
    while (i < s.len && chars.find(s.ptr[i]) != basic_string_view<CharT>::npos) {
        ++i;
    }
    return basic_string_view<CharT>(s.ptr + i, s.len - i);
}

template <typename CharT>
basic_string_view<CharT> trim_right(basic_string_view<CharT> s,
                                    basic_string_view<CharT> chars) noexcept {
    ptrdiff_t n = s.len;
    while (n > 0 && chars.find(s.ptr[n - 1]) != basic_string_view<CharT>::npos) {
        --n;
    }
    return basic_string_view<CharT>(s.ptr, n);
}

template <typename CharT>
basic_string_view<CharT> trim(basic_string_view<CharT> s,
                              basic_string_view<CharT> chars) noexcept {
    return trim_right(trim_left(s, chars), chars);
}

template <typename CharT>
basic_string_view<CharT> trim_left(basic_string_view<CharT> s) noexcept {
    return trim_left(s, static_cast<CharT>(' '));
}

template <typename CharT>
basic_string_view<CharT> trim_right(basic_string_view<CharT> s) noexcept {
    return trim_right(s, static_cast<CharT>(' '));
}

template <typename CharT>
basic_string_view<CharT> trim(basic_string_view<CharT> s) noexcept {
    return trim(s, static_cast<CharT>(' '));
}

template <typename CharT>
basic_string_view<CharT> trim_left(span<const CharT> s, CharT c) noexcept {
    return trim_left(basic_string_view<CharT>(s.ptr, s.len), c);
}

template <typename CharT>
basic_string_view<CharT> trim_right(span<const CharT> s, CharT c) noexcept {
    return trim_right(basic_string_view<CharT>(s.ptr, s.len), c);
}

template <typename CharT>
basic_string_view<CharT> trim(span<const CharT> s, CharT c) noexcept {
    return trim(basic_string_view<CharT>(s.ptr, s.len), c);
}

template <typename CharT>
basic_string_view<CharT> trim_left(span<const CharT> s,
                                   basic_string_view<CharT> chars) noexcept {
    return trim_left(basic_string_view<CharT>(s.ptr, s.len), chars);
}

template <typename CharT>
basic_string_view<CharT> trim_right(span<const CharT> s,
                                    basic_string_view<CharT> chars) noexcept {
    return trim_right(basic_string_view<CharT>(s.ptr, s.len), chars);
}

template <typename CharT>
basic_string_view<CharT> trim(span<const CharT> s,
                              basic_string_view<CharT> chars) noexcept {
    return trim(basic_string_view<CharT>(s.ptr, s.len), chars);
}

template <typename CharT>
basic_string_view<CharT> trim_left(span<const CharT> s) noexcept {
    return trim_left(basic_string_view<CharT>(s.ptr, s.len));
}

template <typename CharT>
basic_string_view<CharT> trim_right(span<const CharT> s) noexcept {
    return trim_right(basic_string_view<CharT>(s.ptr, s.len));
}

template <typename CharT>
basic_string_view<CharT> trim(span<const CharT> s) noexcept {
    return trim(basic_string_view<CharT>(s.ptr, s.len));
}

template <typename CharT, typename Fn>
void split(basic_string_view<CharT> input, CharT delim, Fn on_part) {
    if (input.len == 0) {
        return;
    }
    ptrdiff_t start = 0;
    for (ptrdiff_t i = 0; i < input.len; ++i) {
        if (input.ptr[i] == delim) {
            on_part(basic_string_view<CharT>(input.ptr + start, i - start));
            start = i + 1;
        }
    }
    on_part(basic_string_view<CharT>(input.ptr + start, input.len - start));
}

template <typename CharT, typename Fn>
void split(span<const CharT> input, CharT delim, Fn on_part) {
    split(basic_string_view<CharT>(input.ptr, input.len), delim,
          [&](basic_string_view<CharT> part) { on_part(part); });
}

template <typename CharT>
df::vector<basic_string_view<CharT>> split(basic_string_view<CharT> input, CharT delim) {
    df::vector<basic_string_view<CharT>> parts;
    if (input.len == 0) {
        return parts;
    }
    ptrdiff_t part_count = 1;
    for (ptrdiff_t i = 0; i < input.len; ++i) {
        if (input.ptr[i] == delim) {
            ++part_count;
        }
    }
    parts.reserve(part_count);
    split(input, delim, [&](basic_string_view<CharT> part) {
        parts.push_back(part);
    });
    return parts;
}

template <typename CharT>
df::vector<basic_string_view<CharT>> split(span<const CharT> input, CharT delim) {
    return split(basic_string_view<CharT>(input.ptr, input.len), delim);
}

template <typename CharT, typename Parts>
void join(basic_string_builder<CharT>& builder,
          const Parts& parts,
          basic_string_view<CharT> sep) {
    bool first = true;
    for (const auto& part : parts) {
        if (!first) {
            builder.append(sep);
        }
        first = false;
        builder.append(part);
    }
}

template <typename CharT, typename Parts>
df::basic_string<CharT> join(const Parts& parts, basic_string_view<CharT> sep) {
    basic_string_builder<CharT> builder;
    join(builder, parts, sep);
    return builder.str();
}

} // namespace df