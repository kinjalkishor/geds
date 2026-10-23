#pragma once

// bc_ring_deque.h
//
// Fixed-capacity ring-buffer deque<T>.
// - member names: ptr, head, len, cap
// - no growth: reserve is init-once; overflow asserts
// - allocates through df::allocate / df::deallocate
// - ptrdiff_t internal size; size() unsigned, ssize() signed
// - snake_case, One True Brace Style, template param T

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

template <typename T>
class ring_deque {
public:
    using value_type      = T;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;

    ring_deque() noexcept : ptr(nullptr), head(0), len(0), cap(0) {}

    explicit ring_deque(ptrdiff_t capacity) : ptr(nullptr), head(0), len(0), cap(0) {
        init(capacity);
    }

    ring_deque(const ring_deque&) = delete;
    ring_deque& operator=(const ring_deque&) = delete;

    ring_deque(ring_deque&& other) noexcept
        : ptr(other.ptr), head(other.head), len(other.len), cap(other.cap) {
        other.ptr = nullptr;
        other.head = 0;
        other.len = 0;
        other.cap = 0;
    }

    ring_deque& operator=(ring_deque&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        destroy_all();
        ptr = other.ptr;
        head = other.head;
        len = other.len;
        cap = other.cap;
        other.ptr = nullptr;
        other.head = 0;
        other.len = 0;
        other.cap = 0;
        return *this;
    }

    ~ring_deque() {
        destroy_all();
    }

    void init(ptrdiff_t capacity) {
        assert(len == 0 && "cannot reinit a non-empty ring_deque");
        destroy_all();
        if (capacity <= 0) {
            return;
        }
        void* raw = df::allocate(static_cast<size_t>(capacity) * sizeof(T));
        assert(raw != nullptr && "ring_deque allocation failed");
        ptr = static_cast<T*>(raw);
        head = 0;
        cap = capacity;
    }

    T& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[phys(i)];
    }
    const T& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[phys(i)];
    }

    T& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len && "ring_deque::at out of range");
        return ptr[phys(i)];
    }
    const T& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "ring_deque::at out of range");
        return ptr[phys(i)];
    }

    T& front() noexcept { assert(len > 0); return ptr[head]; }
    const T& front() const noexcept { assert(len > 0); return ptr[head]; }

    T& back() noexcept { assert(len > 0); return ptr[phys(len - 1)]; }
    const T& back() const noexcept { assert(len > 0); return ptr[phys(len - 1)]; }

    size_t size() const noexcept { return static_cast<size_t>(len); }
    ptrdiff_t ssize() const noexcept { return len; }
    size_t capacity() const noexcept { return static_cast<size_t>(cap); }
    ptrdiff_t scapacity() const noexcept { return cap; }
    bool empty() const noexcept { return len == 0; }
    bool full() const noexcept { return len == cap; }

    bool push_back(const T& v) {
        if (len == cap) {
            assert(false && "ring_deque::push_back on full ring");
            return false;
        }
        new (ptr + phys(len)) T(v);
        ++len;
        return true;
    }

    bool push_back(T&& v) {
        if (len == cap) {
            assert(false && "ring_deque::push_back on full ring");
            return false;
        }
        new (ptr + phys(len)) T(std::move(v));
        ++len;
        return true;
    }

    template <typename... Args>
    bool emplace_back(Args&&... args) {
        if (len == cap) {
            assert(false && "ring_deque::emplace_back on full ring");
            return false;
        }
        new (ptr + phys(len)) T(std::forward<Args>(args)...);
        ++len;
        return true;
    }

    bool push_front(const T& v) {
        if (len == cap) {
            assert(false && "ring_deque::push_front on full ring");
            return false;
        }
        head = (head - 1 + cap) % cap;
        new (ptr + head) T(v);
        ++len;
        return true;
    }

    bool push_front(T&& v) {
        if (len == cap) {
            assert(false && "ring_deque::push_front on full ring");
            return false;
        }
        head = (head - 1 + cap) % cap;
        new (ptr + head) T(std::move(v));
        ++len;
        return true;
    }

    template <typename... Args>
    bool emplace_front(Args&&... args) {
        if (len == cap) {
            assert(false && "ring_deque::emplace_front on full ring");
            return false;
        }
        head = (head - 1 + cap) % cap;
        new (ptr + head) T(std::forward<Args>(args)...);
        ++len;
        return true;
    }

    void pop_back() noexcept {
        assert(len > 0);
        --len;
        ptr[phys(len)].~T();
    }

    void pop_front() noexcept {
        assert(len > 0);
        ptr[head].~T();
        head = (head + 1) % cap;
        --len;
    }

    void clear() noexcept {
        while (len > 0) {
            pop_front();
        }
    }

private:
    T* ptr;
    ptrdiff_t head;
    ptrdiff_t len;
    ptrdiff_t cap;

    ptrdiff_t phys(ptrdiff_t logical) const noexcept {
        const ptrdiff_t p = head + logical;
        return p >= cap ? p - cap : p;
    }

    void destroy_all() noexcept {
        clear();
        if (ptr != nullptr) {
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(T));
            ptr = nullptr;
        }
        head = 0;
        cap = 0;
    }
};

} // namespace df