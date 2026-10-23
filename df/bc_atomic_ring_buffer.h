#pragma once

// bc_atomic_ring_buffer.h
//
// Lock-free SPSC (single-producer / single-consumer) ring buffer for
// real-time cross-thread pipelines.
//
// Contract: exactly ONE producer thread calls push / emplace; exactly ONE
// consumer thread calls pop / try_front / advance_read. read_pos and
// write_pos are std::atomic<ptrdiff_t> with the appropriate acquire /
// release ordering. Two producers or two consumers is undefined behavior.
//
// push returns false on full (never overwrites unread data). pop returns
// false on empty.
//
// Style: snake_case, One True Brace Style, template param T.

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>
#include <atomic>

namespace df {

template <typename T>
class atomic_ring_buffer {
public:
    using index_type = ptrdiff_t;

    atomic_ring_buffer() noexcept = default;

    explicit atomic_ring_buffer(ptrdiff_t capacity) {
        init(capacity);
    }

    atomic_ring_buffer(const atomic_ring_buffer&) = delete;
    atomic_ring_buffer& operator=(const atomic_ring_buffer&) = delete;

    atomic_ring_buffer(atomic_ring_buffer&&) = delete;
    atomic_ring_buffer& operator=(atomic_ring_buffer&&) = delete;

    ~atomic_ring_buffer() {
        release();
    }

    void init(ptrdiff_t capacity) {
        assert(capacity >= 0);
        release();
        if (capacity == 0) {
            return;
        }
        void* raw = df::allocate(static_cast<size_t>(capacity) * sizeof(T));
        assert(raw != nullptr && "atomic_ring_buffer: allocation failed");
        ptr_ = static_cast<T*>(raw);
        cap_ = capacity;
        for (ptrdiff_t i = 0; i < cap_; ++i) {
            new (ptr_ + i) T();
        }
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    void release() noexcept {
        if (ptr_ != nullptr) {
            for (ptrdiff_t i = 0; i < cap_; ++i) {
                ptr_[i].~T();
            }
            df::deallocate(ptr_, static_cast<size_t>(cap_) * sizeof(T));
            ptr_ = nullptr;
        }
        cap_ = 0;
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    ptrdiff_t capacity() const noexcept {
        return cap_;
    }

    size_t scapacity() const noexcept {
        return static_cast<size_t>(cap_);
    }

    ptrdiff_t size() const noexcept {
        const ptrdiff_t w = write_pos_.load(std::memory_order_acquire);
        const ptrdiff_t r = read_pos_.load(std::memory_order_acquire);
        ptrdiff_t n = w - r;
        if (n < 0) {
            n += cap_;
        }
        return n;
    }

    size_t size_unsigned() const noexcept {
        return static_cast<size_t>(size());
    }

    bool empty() const noexcept {
        return size() == 0;
    }

    bool full() const noexcept {
        return size() == cap_;
    }

    bool push(const T& value) {
        const ptrdiff_t w = write_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t r = read_pos_.load(std::memory_order_acquire);
        const ptrdiff_t next_w = (w + 1 == cap_) ? 0 : (w + 1);
        if (next_w == r) {
            return false;
        }
        ptr_[w] = value;
        write_pos_.store(next_w, std::memory_order_release);
        return true;
    }

    bool push(T&& value) {
        const ptrdiff_t w = write_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t r = read_pos_.load(std::memory_order_acquire);
        const ptrdiff_t next_w = (w + 1 == cap_) ? 0 : (w + 1);
        if (next_w == r) {
            return false;
        }
        ptr_[w] = std::move(value);
        write_pos_.store(next_w, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    bool emplace(Args&&... args) {
        const ptrdiff_t w = write_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t r = read_pos_.load(std::memory_order_acquire);
        const ptrdiff_t next_w = (w + 1 == cap_) ? 0 : (w + 1);
        if (next_w == r) {
            return false;
        }
        ptr_[w] = T(std::forward<Args>(args)...);
        write_pos_.store(next_w, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const ptrdiff_t r = read_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t w = write_pos_.load(std::memory_order_acquire);
        if (r == w) {
            return false;
        }
        out = std::move(ptr_[r]);
        const ptrdiff_t next_r = (r + 1 == cap_) ? 0 : (r + 1);
        read_pos_.store(next_r, std::memory_order_release);
        return true;
    }

    T* try_front() noexcept {
        const ptrdiff_t r = read_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t w = write_pos_.load(std::memory_order_acquire);
        if (r == w) {
            return nullptr;
        }
        return &ptr_[r];
    }

    bool advance_read() noexcept {
        const ptrdiff_t r = read_pos_.load(std::memory_order_relaxed);
        const ptrdiff_t w = write_pos_.load(std::memory_order_acquire);
        if (r == w) {
            return false;
        }
        const ptrdiff_t next_r = (r + 1 == cap_) ? 0 : (r + 1);
        read_pos_.store(next_r, std::memory_order_release);
        return true;
    }

    index_type write_pos() const noexcept {
        return write_pos_.load(std::memory_order_acquire);
    }

    index_type read_pos() const noexcept {
        return read_pos_.load(std::memory_order_acquire);
    }

private:
    T* ptr_ = nullptr;
    ptrdiff_t cap_ = 0;
    std::atomic<ptrdiff_t> write_pos_{0};
    std::atomic<ptrdiff_t> read_pos_{0};
};

} // namespace df