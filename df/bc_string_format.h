#pragma once

// bc_string_format.h
//
// printf-style formatting into a df::string. One-shot convenience wrapper
// around the two-pass snprintf pattern used by basic_string_builder.
//
// Style: snake_case, One True Brace Style.

#include "bc_string.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

namespace df {

inline df::string format(const char* fmt, ...) {
    assert(fmt != nullptr && "df::format: fmt is null");
    va_list args;
    va_start(args, fmt);

    va_list args_copy;
    va_copy(args_copy, args);
    const int needed = vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    df::string result;
    if (needed > 0) {
        result.resize(static_cast<ptrdiff_t>(needed));
        vsnprintf(result.data(), static_cast<size_t>(needed + 1), fmt, args);
    }
    va_end(args);
    return result;
}

inline df::string vformat(const char* fmt, va_list args) {
    assert(fmt != nullptr && "df::vformat: fmt is null");
    va_list args_copy;
    va_copy(args_copy, args);
    const int needed = vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    df::string result;
    if (needed > 0) {
        result.resize(static_cast<ptrdiff_t>(needed));
        vsnprintf(result.data(), static_cast<size_t>(needed + 1), fmt, args);
    }
    return result;
}

} // namespace df