#pragma once

// bc_span.h
//
// Non-owning view over a contiguous sequence of T. The (ptr, len) pair as
// a first-class type. Implicit conversion from any container with .data()
// and .size() — df::vector<T>, df::basic_string<CharT>,
// df::stack_vector<T, N>, df::stack_basic_string<T, N>, std::vector<T>,
// std::array<T, N>, and any user type that provides those two accessors.
// Also constructible from a raw (T*, ptrdiff_t) pair and from a C array.
//
// No ownership. Does not allocate, does not free.
//
// Style: snake_case, One True Brace Style, template param T.

#include <stddef.h>
#include <assert.h>

namespace df {

template <typename T>
class span {
public:
    using value_type = T;
    using size_type  = size_t;
    using reference  = T&;
    using const_reference = const T&;
    using pointer    = T*;
    using const_pointer = const T*;
    using iterator   = T*;
    using const_iterator = const T*;

    T* ptr = nullptr;
    ptrdiff_t len = 0;

    constexpr span() noexcept = default;

    constexpr span(T* p, ptrdiff_t n) noexcept
        : ptr(p), len(n) {}

    template <ptrdiff_t N>
    constexpr span(T (&arr)[N]) noexcept
        : ptr(arr), len(N) {}

    template <typename Container>
    constexpr span(Container& c) noexcept
        : ptr(c.data()), len(static_cast<ptrdiff_t>(c.size())) {}

    template <typename Container>
    constexpr span(const Container& c) noexcept
        : ptr(c.data()), len(static_cast<ptrdiff_t>(c.size())) {}

    constexpr size_t size() const noexcept {
        return static_cast<size_t>(len);
    }

    constexpr ptrdiff_t ssize() const noexcept {
        return len;
    }

    constexpr bool empty() const noexcept {
        return len == 0;
    }

    constexpr T* data() const noexcept {
        return ptr;
    }

    constexpr T* begin() const noexcept {
        return ptr;
    }

    constexpr T* end() const noexcept {
        return ptr + len;
    }

    constexpr const T* cbegin() const noexcept {
        return ptr;
    }

    constexpr const T* cend() const noexcept {
        return ptr + len;
    }

    T& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    T& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "span::at out of range");
        return ptr[i];
    }

    T& front() const noexcept {
        assert(len > 0);
        return ptr[0];
    }

    T& back() const noexcept {
        assert(len > 0);
        return ptr[len - 1];
    }

    constexpr span<T> first(ptrdiff_t n) const noexcept {
        assert(n >= 0 && n <= len && "span::first out of range");
        return span<T>(ptr, n);
    }

    constexpr span<T> last(ptrdiff_t n) const noexcept {
        assert(n >= 0 && n <= len && "span::last out of range");
        return span<T>(ptr + (len - n), n);
    }

    constexpr span<T> subspan(ptrdiff_t pos, ptrdiff_t n = -1) const noexcept {
        assert(pos >= 0 && pos <= len && "span::subspan pos out of range");
        const ptrdiff_t remaining = len - pos;
        const ptrdiff_t take = (n < 0 || n > remaining) ? remaining : n;
        return span<T>(ptr + pos, take);
    }
};

} // namespace df