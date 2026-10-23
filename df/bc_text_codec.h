#pragma once

// bc_text_codec.h
//
// UTF-8 / UTF-16 / UTF-32 conversion and code-point iteration over byte
// spans. The "characters" layer that utf8_string intentionally omits.
//
// Symmetric convenience wrappers exist for the three directions:
//   to_utf8(span<const char32_t>)  / to_utf8(span<const char16_t>)
//   to_utf16(span<const char>)     / to_utf32(span<const char>)
//   from_utf8_to_utf16 / from_utf8_to_utf32   (long-form aliases)
//
// Style: snake_case, One True Brace Style, bare size_t / ptrdiff_t / uintN_t.

#include "bc_utf8_string.h"
#include "bc_string.h"
#include "bc_span.h"
#include "bc_vector.h"

#include <stddef.h>
#include <stdint.h>

namespace df {

namespace detail {

inline constexpr char32_t replacement_character = 0xFFFDu;

inline char32_t decode_utf8_at(span<const char> bytes, ptrdiff_t& byte_offset) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(bytes[byte_offset]);
    if (b0 < 0x80) {
        ++byte_offset;
        return static_cast<char32_t>(b0);
    }
    ptrdiff_t continuation = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        continuation = 1;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        continuation = 2;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        continuation = 3;
        cp = b0 & 0x07;
    } else {
        ++byte_offset;
        return replacement_character;
    }
    if (byte_offset + continuation >= bytes.ssize()) {
        ++byte_offset;
        return replacement_character;
    }
    for (ptrdiff_t c = 1; c <= continuation; ++c) {
        const unsigned char cb = static_cast<unsigned char>(bytes[byte_offset + c]);
        if ((cb & 0xC0) != 0x80) {
            ++byte_offset;
            return replacement_character;
        }
        cp = (cp << 6) | (cb & 0x3F);
    }
    byte_offset += continuation + 1;
    return cp;
}

inline char32_t decode_utf16_at(span<const char16_t> units, ptrdiff_t& unit_offset) noexcept {
    const char16_t u0 = units[unit_offset];
    if (u0 < 0xD800 || u0 > 0xDFFF) {
        ++unit_offset;
        return static_cast<char32_t>(u0);
    }
    if (u0 >= 0xDC00) {
        ++unit_offset;
        return replacement_character;
    }
    if (unit_offset + 1 >= units.ssize()) {
        ++unit_offset;
        return replacement_character;
    }
    const char16_t u1 = units[unit_offset + 1];
    if (u1 < 0xDC00 || u1 > 0xDFFF) {
        ++unit_offset;
        return replacement_character;
    }
    unit_offset += 2;
    return 0x10000u + ((static_cast<char32_t>(u0) - 0xD800u) << 10)
                    + (static_cast<char32_t>(u1) - 0xDC00u);
}

inline int utf8_bytes_for(char32_t cp) noexcept {
    if (cp < 0x80)    return 1;
    if (cp < 0x800)   return 2;
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        return 3;
    }
    if (cp <= 0x10FFFF) return 4;
    return 0;
}

inline ptrdiff_t encode_utf8_into(char32_t cp, char* out, ptrdiff_t out_index) noexcept {
    const int n = utf8_bytes_for(cp);
    if (n == 0) {
        out[out_index]     = static_cast<char>(0xEF);
        out[out_index + 1] = static_cast<char>(0xBF);
        out[out_index + 2] = static_cast<char>(0xBD);
        return 3;
    }
    switch (n) {
        case 1:
            out[out_index] = static_cast<char>(cp);
            return 1;
        case 2:
            out[out_index]     = static_cast<char>(0xC0 | (cp >> 6));
            out[out_index + 1] = static_cast<char>(0x80 | (cp & 0x3F));
            return 2;
        case 3:
            out[out_index]     = static_cast<char>(0xE0 | (cp >> 12));
            out[out_index + 1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[out_index + 2] = static_cast<char>(0x80 | (cp & 0x3F));
            return 3;
        default:
            out[out_index]     = static_cast<char>(0xF0 | (cp >> 18));
            out[out_index + 1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out[out_index + 2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[out_index + 3] = static_cast<char>(0x80 | (cp & 0x3F));
            return 4;
    }
}

} // namespace detail

inline bool utf8_is_valid(span<const char> bytes) noexcept {
    ptrdiff_t i = 0;
    const ptrdiff_t n = bytes.ssize();
    while (i < n) {
        const unsigned char b = static_cast<unsigned char>(bytes[i]);
        if (b == 0x00) {
            return false;
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
            return false;
        }
        if (i + continuation >= n) {
            return false;
        }
        for (ptrdiff_t c = 1; c <= continuation; ++c) {
            const unsigned char cb = static_cast<unsigned char>(bytes[i + c]);
            if ((cb & 0xC0) != 0x80) {
                return false;
            }
        }
        i += continuation + 1;
    }
    return true;
}

inline bool utf16_is_valid(span<const char16_t> units) noexcept {
    ptrdiff_t i = 0;
    const ptrdiff_t n = units.ssize();
    while (i < n) {
        const char16_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= n) {
                return false;
            }
            const char16_t u2 = units[i + 1];
            if (u2 < 0xDC00 || u2 > 0xDFFF) {
                return false;
            }
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;
        } else {
            ++i;
        }
    }
    return true;
}

inline ptrdiff_t utf8_code_point_count(span<const char> bytes) noexcept {
    ptrdiff_t count = 0;
    ptrdiff_t i = 0;
    const ptrdiff_t n = bytes.ssize();
    while (i < n) {
        const unsigned char b = static_cast<unsigned char>(bytes[i]);
        if (b < 0x80) {
            i += 1;
        } else if ((b & 0xE0) == 0xC0) {
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
        if (i > n) {
            i = n;
        }
        ++count;
    }
    return count;
}

inline ptrdiff_t utf16_code_point_count(span<const char16_t> units) noexcept {
    ptrdiff_t count = 0;
    ptrdiff_t i = 0;
    const ptrdiff_t n = units.ssize();
    while (i < n) {
        const char16_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            const char16_t u2 = units[i + 1];
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                i += 2;
                ++count;
                continue;
            }
        }
        ++i;
        ++count;
    }
    return count;
}

inline char32_t utf8_next_code_point(span<const char> bytes, ptrdiff_t& byte_offset) noexcept {
    assert(byte_offset >= 0 && byte_offset < bytes.ssize());
    return detail::decode_utf8_at(bytes, byte_offset);
}

inline char32_t utf16_next_code_point(span<const char16_t> units, ptrdiff_t& unit_offset) noexcept {
    assert(unit_offset >= 0 && unit_offset < units.ssize());
    return detail::decode_utf16_at(units, unit_offset);
}

inline utf8_string to_utf8(span<const char32_t> code_points) {
    utf8_string result;
    result.reserve(code_points.ssize() * 4);
    char buf[4];
    for (ptrdiff_t i = 0; i < code_points.ssize(); ++i) {
        const ptrdiff_t n = detail::encode_utf8_into(code_points[i], buf, 0);
        result.append(buf, n);
    }
    return result;
}

inline utf8_string to_utf8(span<const char16_t> code_units) {
    utf8_string result;
    result.reserve(code_units.ssize() * 3);
    char buf[4];
    ptrdiff_t i = 0;
    while (i < code_units.ssize()) {
        const char32_t cp = detail::decode_utf16_at(code_units, i);
        const ptrdiff_t n = detail::encode_utf8_into(cp, buf, 0);
        result.append(buf, n);
    }
    return result;
}

inline u32string from_utf8_to_utf32(span<const char> bytes) {
    u32string result;
    result.reserve(utf8_code_point_count(bytes));
    ptrdiff_t i = 0;
    while (i < bytes.ssize()) {
        const char32_t cp = detail::decode_utf8_at(bytes, i);
        result.push_back(cp);
    }
    return result;
}

inline u16string from_utf8_to_utf16(span<const char> bytes) {
    u16string result;
    result.reserve(bytes.ssize());
    ptrdiff_t i = 0;
    while (i < bytes.ssize()) {
        const char32_t cp = detail::decode_utf8_at(bytes, i);
        if (cp < 0x10000) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                result.push_back(static_cast<char16_t>(detail::replacement_character));
            } else {
                result.push_back(static_cast<char16_t>(cp));
            }
        } else {
            const char32_t v = cp - 0x10000u;
            result.push_back(static_cast<char16_t>(0xD800u + (v >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00u + (v & 0x3FFu)));
        }
    }
    return result;
}

inline u32string to_utf32(span<const char> bytes) {
    return from_utf8_to_utf32(bytes);
}

inline u16string to_utf16(span<const char> bytes) {
    return from_utf8_to_utf16(bytes);
}

} // namespace df