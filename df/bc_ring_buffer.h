#pragma once

// bc_ring_buffer.h
//
// Fixed-capacity circular array with a monotonic write cursor and
// offset-based read access. Not a queue. Nothing is popped; the whole
// history up to capacity() slots is always addressable. Writing when
// full overwrites the oldest slot.
//
// Use cases: history / streaming I/O / physics rollback / telemetry.
//
// Not for: FIFO queues (use ring_deque), cross-thread pipelines (use
// atomic_ring_buffer).
//
// Style: snake_case, One True Brace Style, template param T.

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>

namespace df {

template <typename T>
class ring_buffer {
public:
    using index_type = ptrdiff_t;

    ring_buffer() noexcept = default;

    explicit ring_buffer(ptrdiff_t capacity) {
        init(capacity);
    }

    ring_buffer(const ring_buffer&) = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;

    ring_buffer(ring_buffer&& other) noexcept
        : ptr(other.ptr),
          cap(other.cap),
          write_pos_(other.write_pos_),
          filled_(other.filled_) {
        other.ptr = nullptr;
        other.cap = 0;
        other.write_pos_ = 0;
        other.filled_ = 0;
    }

    ring_buffer& operator=(ring_buffer&& other) noexcept {
        if (this != &other) {
            release();
            ptr = other.ptr;
            cap = other.cap;
            write_pos_ = other.write_pos_;
            filled_ = other.filled_;
            other.ptr = nullptr;
            other.cap = 0;
            other.write_pos_ = 0;
            other.filled_ = 0;
        }
        return *this;
    }

    ~ring_buffer() {
        release();
    }

    void init(ptrdiff_t capacity) {
        assert(capacity >= 0);
        release();
        if (capacity == 0) {
            return;
        }
        void* raw = df::allocate(static_cast<size_t>(capacity) * sizeof(T));
        assert(raw != nullptr && "ring_buffer: allocation failed");
        ptr = static_cast<T*>(raw);
        cap = capacity;
        for (ptrdiff_t i = 0; i < cap; ++i) {
            new (ptr + i) T();
        }
        write_pos_ = 0;
        filled_ = 0;
    }

    void release() noexcept {
        if (ptr != nullptr) {
            for (ptrdiff_t i = 0; i < cap; ++i) {
                ptr[i].~T();
            }
            df::deallocate(ptr, static_cast<size_t>(cap) * sizeof(T));
            ptr = nullptr;
        }
        cap = 0;
        write_pos_ = 0;
        filled_ = 0;
    }

    ptrdiff_t capacity() const noexcept {
        return cap;
    }

    size_t scapacity() const noexcept {
        return static_cast<size_t>(cap);
    }

    ptrdiff_t size() const noexcept {
        return filled_;
    }

    size_t size_unsigned() const noexcept {
        return static_cast<size_t>(filled_);
    }

    bool empty() const noexcept {
        return filled_ == 0;
    }

    bool full() const noexcept {
        return filled_ >= cap;
    }

    index_type write_pos() const noexcept {
        return write_pos_;
    }

    T& push(const T& value) {
        T& slot = slot_at_physical(write_pos_ % cap);
        slot = value;
        ++write_pos_;
        if (filled_ < cap) {
            ++filled_;
        }
        return slot;
    }

    T& push(T&& value) {
        T& slot = slot_at_physical(write_pos_ % cap);
        slot = std::move(value);
        ++write_pos_;
        if (filled_ < cap) {
            ++filled_;
        }
        return slot;
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        T& slot = slot_at_physical(write_pos_ % cap);
        slot = T(std::forward<Args>(args)...);
        ++write_pos_;
        if (filled_ < cap) {
            ++filled_;
        }
        return slot;
    }

    T& back_from_write(index_type lag) noexcept {
        assert(lag >= 0 && lag < filled_ && "ring_buffer::back_from_write out of range");
        return at_physical((write_pos_ - 1 - lag) % cap);
    }

    const T& back_from_write(index_type lag) const noexcept {
        assert(lag >= 0 && lag < filled_ && "ring_buffer::back_from_write out of range");
        return at_physical((write_pos_ - 1 - lag) % cap);
    }

    T& at_physical(index_type physical_idx) noexcept {
        assert(physical_idx >= 0 && physical_idx < cap);
        return ptr[physical_idx];
    }

    const T& at_physical(index_type physical_idx) const noexcept {
        assert(physical_idx >= 0 && physical_idx < cap);
        return ptr[physical_idx];
    }

    T& slot_at(index_type physical_idx) noexcept {
        return at_physical(physical_idx);
    }

    const T& slot_at(index_type physical_idx) const noexcept {
        return at_physical(physical_idx);
    }

    T* begin() noexcept {
        if (filled_ == 0) {
            return nullptr;
        }
        if (filled_ < cap) {
            return ptr;
        }
        return ptr + (write_pos_ % cap);
    }

    T* end() noexcept {
        if (filled_ == 0) {
            return nullptr;
        }
        if (filled_ < cap) {
            return ptr + filled_;
        }
        return begin();
    }

    const T* begin() const noexcept {
        if (filled_ == 0) {
            return nullptr;
        }
        if (filled_ < cap) {
            return ptr;
        }
        return ptr + (write_pos_ % cap);
    }

    const T* end() const noexcept {
        if (filled_ == 0) {
            return nullptr;
        }
        if (filled_ < cap) {
            return ptr + filled_;
        }
        return begin();
    }

    void clear() noexcept {
        write_pos_ = 0;
        filled_ = 0;
    }

    void swap(ring_buffer& other) noexcept {
        std::swap(ptr,        other.ptr);
        std::swap(cap,        other.cap);
        std::swap(write_pos_, other.write_pos_);
        std::swap(filled_,    other.filled_);
    }

private:
    T* ptr = nullptr;
    ptrdiff_t cap = 0;
    ptrdiff_t write_pos_ = 0;
    ptrdiff_t filled_ = 0;

    T& slot_at_physical(index_type physical_idx) noexcept {
        return ptr[physical_idx];
    }
};

template <typename T>
inline void swap(ring_buffer<T>& a, ring_buffer<T>& b) noexcept {
    a.swap(b);
}

} // namespace df