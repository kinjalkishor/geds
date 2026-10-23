#pragma once

// bc_vector.h
//
// Lightweight custom vector<T> for the game engine.
// - member names: ptr, len, cap
// - allocates through df::allocate / df::deallocate / df::reallocate
//   (pool-backed when a pool is registered on the thread, malloc otherwise)
// - no allocator template parameter; template only on T
// - ptrdiff_t internal size; size() unsigned, ssize() signed
// - std-compatible API surface, plus emplace, insert(range), operator==, <
// - contains / find (find returns T* with nullptr on miss)
// - exception safety:
//     * trivially copyable/destructible T: memcpy/memmove fast paths,
//       nothrow in practice (allocation failure asserts)
//     * non-trivial T: strong guarantee on single-element insert/emplace via
//       a slot guard; strong guarantee on range insert via rebuild-and-swap
// - snake_case, One True Brace Style, template params T / U

#include "bc_allocator.h"

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <new>
#include <utility>
#include <initializer_list>
#include <iterator>
#include <type_traits>

namespace df {

template <typename T>
class vector {
public:
    using value_type      = T;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = T*;
    using const_iterator  = const T*;

    static constexpr ptrdiff_t min_cap = 8;

private:
    static constexpr bool trivially_relocatable =
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

    static constexpr bool nothrow_shift =
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_assignable_v<T>;

    struct slot_guard {
        T* p;
        bool active;
        ~slot_guard() {
            if (active) {
                p->~T();
            }
        }
    };

public:
    vector() noexcept : ptr(nullptr), len(0), cap(0) {}

    explicit vector(ptrdiff_t n) : ptr(nullptr), len(0), cap(0) {
        if (n > 0) {
            reserve(n);
            for (ptrdiff_t i = 0; i < n; ++i) {
                new (ptr + i) T();
            }
            len = n;
        }
    }

    vector(ptrdiff_t n, const T& value) : ptr(nullptr), len(0), cap(0) {
        if (n > 0) {
            reserve(n);
            for (ptrdiff_t i = 0; i < n; ++i) {
                new (ptr + i) T(value);
            }
            len = n;
        }
    }

    vector(std::initializer_list<T> il) : ptr(nullptr), len(0), cap(0) {
        reserve(static_cast<ptrdiff_t>(il.size()));
        for (const T& v : il) {
            new (ptr + len) T(v);
            ++len;
        }
    }

    template <typename InputIt, typename = decltype(*std::declval<InputIt>())>
    vector(InputIt first, InputIt last) : ptr(nullptr), len(0), cap(0) {
        assign_range(first, last);
    }

    vector(const vector& other) : ptr(nullptr), len(0), cap(0) {
        if (other.len > 0) {
            reserve(other.len);
            for (ptrdiff_t i = 0; i < other.len; ++i) {
                new (ptr + i) T(other.ptr[i]);
            }
            len = other.len;
        }
    }

    vector(vector&& other) noexcept : ptr(other.ptr), len(other.len), cap(other.cap) {
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
    }

    vector& operator=(const vector& other) {
        if (this == &other) {
            return *this;
        }
        vector tmp(other);
        swap(tmp);
        return *this;
    }

    vector& operator=(vector&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        destroy_all();
        ptr = other.ptr;
        len = other.len;
        cap = other.cap;
        other.ptr = nullptr;
        other.len = 0;
        other.cap = 0;
        return *this;
    }

    ~vector() {
        destroy_all();
    }

    T& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }
    const T& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    T& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len && "vector::at out of range");
        return ptr[i];
    }
    const T& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "vector::at out of range");
        return ptr[i];
    }

    T& front() noexcept {
        assert(len > 0);
        return ptr[0];
    }
    const T& front() const noexcept {
        assert(len > 0);
        return ptr[0];
    }

    T& back() noexcept {
        assert(len > 0);
        return ptr[len - 1];
    }
    const T& back() const noexcept {
        assert(len > 0);
        return ptr[len - 1];
    }

    T* data() noexcept { return ptr; }
    const T* data() const noexcept { return ptr; }

    iterator begin() noexcept { return ptr; }
    iterator end()   noexcept { return ptr + len; }
    const_iterator begin() const noexcept { return ptr; }
    const_iterator end()   const noexcept { return ptr + len; }
    const_iterator cbegin() const noexcept { return ptr; }
    const_iterator cend()   const noexcept { return ptr + len; }

    size_t size() const noexcept {
        return static_cast<size_t>(len);
    }
    ptrdiff_t ssize() const noexcept {
        return len;
    }
    size_t capacity() const noexcept {
        return static_cast<size_t>(cap);
    }
    ptrdiff_t scapacity() const noexcept {
        return cap;
    }
    bool empty() const noexcept {
        return len == 0;
    }

    bool contains(const T& value) const {
        for (ptrdiff_t i = 0; i < len; ++i) {
            if (ptr[i] == value) {
                return true;
            }
        }
        return false;
    }

    T* find(const T& value) noexcept {
        for (ptrdiff_t i = 0; i < len; ++i) {
            if (ptr[i] == value) {
                return &ptr[i];
            }
        }
        return nullptr;
    }
    const T* find(const T& value) const noexcept {
        for (ptrdiff_t i = 0; i < len; ++i) {
            if (ptr[i] == value) {
                return &ptr[i];
            }
        }
        return nullptr;
    }

    void reserve(ptrdiff_t new_cap) {
        if (new_cap <= cap) {
            return;
        }
        reallocate_to(new_cap);
    }

    void resize(ptrdiff_t new_len) {
        resize(new_len, T{});
    }

    void resize(ptrdiff_t new_len, const T& value) {
        assert(new_len >= 0);
        if (new_len < len) {
            destroy_range(new_len, len);
            len = new_len;
        } else if (new_len > len) {
            if (new_len > cap) {
                reserve(grow_to(new_len));
            }
            for (ptrdiff_t i = len; i < new_len; ++i) {
                new (ptr + i) T(value);
            }
            len = new_len;
        }
    }

    void shrink_to_fit() {
        if (cap > len) {
            if (len == 0) {
                destroy_all();
            } else {
                reallocate_to(len);
            }
        }
    }

    void clear() noexcept {
        destroy_range(0, len);
        len = 0;
    }

    void push_back(const T& value) {
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        new (ptr + len) T(value);
        ++len;
    }

    void push_back(T&& value) {
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        new (ptr + len) T(std::move(value));
        ++len;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        new (ptr + len) T(std::forward<Args>(args)...);
        ++len;
        return ptr[len - 1];
    }

    void pop_back() noexcept {
        assert(len > 0);
        --len;
        ptr[len].~T();
    }

    ptrdiff_t insert(ptrdiff_t pos, const T& value) {
        assert(pos >= 0 && pos <= len);
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        return insert_prepared(pos, value);
    }

    ptrdiff_t insert(ptrdiff_t pos, T&& value) {
        assert(pos >= 0 && pos <= len);
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        return insert_prepared(pos, std::move(value));
    }

    template <typename... Args>
    ptrdiff_t emplace(ptrdiff_t pos, Args&&... args) {
        assert(pos >= 0 && pos <= len);
        if (len == cap) {
            reserve(grow_to(len + 1));
        }
        if (pos == len) {
            new (ptr + len) T(std::forward<Args>(args)...);
            ++len;
            return pos;
        }
        T staged(std::forward<Args>(args)...);
        shift_right_and_place(pos, std::move(staged));
        return pos;
    }

    template <typename InputIt, typename = decltype(*std::declval<InputIt>())>
    ptrdiff_t insert(ptrdiff_t pos, InputIt first, InputIt last) {
        assert(pos >= 0 && pos <= len);
        if (first == last) {
            return pos;
        }
        if constexpr (trivially_relocatable) {
            vector tmp;
            for (InputIt it = first; it != last; ++it) {
                tmp.push_back(*it);
            }
            return insert_trivial_range(pos, tmp);
        } else {
            return insert_strong_range(pos, first, last);
        }
    }

    ptrdiff_t erase(ptrdiff_t pos) {
        assert(pos >= 0 && pos < len);
        for (ptrdiff_t i = pos; i < len - 1; ++i) {
            ptr[i] = std::move(ptr[i + 1]);
        }
        --len;
        ptr[len].~T();
        return len;
    }

    ptrdiff_t erase(ptrdiff_t first, ptrdiff_t last) {
        assert(first >= 0 && first <= last && last <= len);
        const ptrdiff_t n = last - first;
        if (n == 0) {
            return len;
        }
        if constexpr (trivially_relocatable) {
            const ptrdiff_t tail = len - last;
            if (tail > 0) {
                memmove(ptr + first,
                        ptr + last,
                        static_cast<size_t>(tail) * sizeof(T));
            }
        } else {
            for (ptrdiff_t i = first; i < len - n; ++i) {
                ptr[i] = std::move(ptr[i + n]);
            }
            for (ptrdiff_t i = len - n; i < len; ++i) {
                ptr[i].~T();
            }
        }
        len -= n;
        return len;
    }

    void swap(vector& other) noexcept {
        std::swap(ptr, other.ptr);
        std::swap(len, other.len);
        std::swap(cap, other.cap);
    }

    template <typename InputIt, typename = decltype(*std::declval<InputIt>())>
    void assign(InputIt first, InputIt last) {
        clear();
        assign_range(first, last);
    }

    void assign(ptrdiff_t n, const T& value) {
        clear();
        reserve(n);
        for (ptrdiff_t i = 0; i < n; ++i) {
            new (ptr + i) T(value);
        }
        len = n;
    }

    bool operator==(const vector& other) const {
        if (len != other.len) {
            return false;
        }
        for (ptrdiff_t i = 0; i < len; ++i) {
            if (!(ptr[i] == other.ptr[i])) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const vector& other) const {
        return !(*this == other);
    }

    bool operator<(const vector& other) const {
        const ptrdiff_t n = (len < other.len) ? len : other.len;
        for (ptrdiff_t i = 0; i < n; ++i) {
            if (ptr[i] < other.ptr[i]) {
                return true;
            }
            if (other.ptr[i] < ptr[i]) {
                return false;
            }
        }
        return len < other.len;
    }

    bool operator>(const vector& other) const { return other < *this; }
    bool operator<=(const vector& other) const { return !(other < *this); }
    bool operator>=(const vector& other) const { return !(*this < other); }

private:
    T* ptr;
    ptrdiff_t len;
    ptrdiff_t cap;

    void destroy_range(ptrdiff_t first, ptrdiff_t last) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (ptrdiff_t i = first; i < last; ++i) {
                ptr[i].~T();
            }
        }
    }

    void destroy_all() noexcept {
        if (ptr != nullptr) {
            destroy_range(0, len);
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(T));
            ptr = nullptr;
        }
        len = 0;
        cap = 0;
    }

    static ptrdiff_t grow_to(ptrdiff_t needed) {
        ptrdiff_t new_cap = cap ? cap : min_cap;
        while (new_cap < needed) {
            new_cap = new_cap + new_cap / 2;
            if (new_cap < min_cap) {
                new_cap = min_cap;
            }
        }
        return new_cap;
    }

    void reallocate_to(ptrdiff_t new_cap) {
        assert(new_cap >= len);
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(T));
        assert(raw != nullptr && "vector allocation failed");
        T* fresh = static_cast<T*>(raw);

        if constexpr (trivially_relocatable) {
            if (len > 0) {
                memcpy(fresh, ptr, static_cast<size_t>(len) * sizeof(T));
            }
        } else {
            for (ptrdiff_t i = 0; i < len; ++i) {
                new (fresh + i) T(std::move_if_noexcept(ptr[i]));
            }
            for (ptrdiff_t i = 0; i < len; ++i) {
                ptr[i].~T();
            }
        }

        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(T));
        }
        ptr = fresh;
        cap = new_cap;
    }

    void shift_right_and_place(ptrdiff_t pos, T&& value) {
        if constexpr (nothrow_shift) {
            if (pos < len) {
                new (ptr + len) T(std::move(ptr[len - 1]));
                for (ptrdiff_t i = len - 1; i > pos; --i) {
                    ptr[i] = std::move(ptr[i - 1]);
                }
                ptr[pos] = std::move(value);
            } else {
                new (ptr + len) T(std::move(value));
            }
            ++len;
        } else {
            if (pos < len) {
                slot_guard g{ptr + len, false};
                new (ptr + len) T(std::move_if_noexcept(ptr[len - 1]));
                g.active = true;
                for (ptrdiff_t i = len - 1; i > pos; --i) {
                    ptr[i] = std::move_if_noexcept(ptr[i - 1]);
                }
                ptr[pos] = std::move_if_noexcept(value);
                g.active = false;
            } else {
                new (ptr + len) T(std::move_if_noexcept(value));
            }
            ++len;
        }
    }

    ptrdiff_t insert_prepared(ptrdiff_t pos, const T& value) {
        if (pos == len) {
            new (ptr + len) T(value);
            ++len;
            return pos;
        }
        T staged(value);
        shift_right_and_place(pos, std::move(staged));
        return pos;
    }

    ptrdiff_t insert_prepared(ptrdiff_t pos, T&& value) {
        if (pos == len) {
            new (ptr + len) T(std::move(value));
            ++len;
            return pos;
        }
        T staged(std::move(value));
        shift_right_and_place(pos, std::move(staged));
        return pos;
    }

    ptrdiff_t insert_trivial_range(ptrdiff_t pos, vector& tmp) {
        const ptrdiff_t n = tmp.len;
        if (n == 0) {
            return pos;
        }
        const ptrdiff_t need = len + n;
        if (need > cap) {
            reserve(grow_to(need));
        }
        const ptrdiff_t tail = len - pos;
        if (tail > 0) {
            memmove(ptr + pos + n,
                    ptr + pos,
                    static_cast<size_t>(tail) * sizeof(T));
        }
        if (n > 0) {
            memcpy(ptr + pos,
                   tmp.ptr,
                   static_cast<size_t>(n) * sizeof(T));
        }
        len += n;
        return pos;
    }

    template <typename InputIt>
    ptrdiff_t insert_strong_range(ptrdiff_t pos, InputIt first, InputIt last) {
        vector fresh;
        fresh.reserve(len + static_cast<ptrdiff_t>(std::distance(first, last)));

        for (ptrdiff_t i = 0; i < pos; ++i) {
            fresh.push_back(std::move_if_noexcept(ptr[i]));
        }
        for (InputIt it = first; it != last; ++it) {
            fresh.push_back(*it);
        }
        for (ptrdiff_t i = pos; i < len; ++i) {
            fresh.push_back(std::move_if_noexcept(ptr[i]));
        }

        swap(fresh);
        return pos;
    }

    template <typename InputIt>
    void assign_range(InputIt first, InputIt last) {
        if constexpr (std::is_same_v<
                typename std::iterator_traits<InputIt>::iterator_category,
                std::random_access_iterator_tag>) {
            const ptrdiff_t n = static_cast<ptrdiff_t>(last - first);
            if (n > 0) {
                reserve(n);
                for (ptrdiff_t i = 0; i < n; ++i) {
                    new (ptr + i) T(first[i]);
                }
                len = n;
            }
        } else {
            for (InputIt it = first; it != last; ++it) {
                push_back(*it);
            }
        }
    }
};

template <typename T>
inline void swap(vector<T>& a, vector<T>& b) noexcept {
    a.swap(b);
}

} // namespace df