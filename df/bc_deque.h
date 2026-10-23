#pragma once

// bc_deque.h
//
// Growable deque<T> backed by fixed-size segments.
// - O(1) amortised push/pop at both ends, no element moves
// - Segment size (in elements) chosen at construction; default 64
// - Segments and the segment-pointer array are pool-backed
// - ptrdiff_t internal size; size() unsigned, ssize() signed
// - No iterators, no middle insert/erase (queue / sequence-of-push-pop use)
// - Allocations route through df::allocate / df::deallocate
// - shrink_to_fit is a no-op for API parity with std::deque
// - swap member + free swap
//
// Malloc-backed counterpart: sdf::deque<T> in bcs_deque.h.
//
// Style: snake_case, One True Brace Style, template param T.

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

template <typename T>
class deque {
public:
    using value_type      = T;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;

    static constexpr ptrdiff_t default_seg_size = 64;

    deque() noexcept
        : segments_(nullptr), seg_count(0), seg_cap(0),
          first_seg(0), first_off(0), len(0), seg_size(default_seg_size) {}

    explicit deque(ptrdiff_t seg_size_)
        : segments_(nullptr), seg_count(0), seg_cap(0),
          first_seg(0), first_off(0), len(0), seg_size(seg_size_ > 0 ? seg_size_ : 1) {}

    deque(const deque&) = delete;
    deque& operator=(const deque&) = delete;

    deque(deque&& other) noexcept
        : segments_(other.segments_), seg_count(other.seg_count), seg_cap(other.seg_cap),
          first_seg(other.first_seg), first_off(other.first_off),
          len(other.len), seg_size(other.seg_size) {
        other.segments_ = nullptr;
        other.seg_count = 0;
        other.seg_cap = 0;
        other.first_seg = 0;
        other.first_off = 0;
        other.len = 0;
    }

    deque& operator=(deque&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        destroy_all();
        segments_ = other.segments_;
        seg_count = other.seg_count;
        seg_cap = other.seg_cap;
        first_seg = other.first_seg;
        first_off = other.first_off;
        len = other.len;
        seg_size = other.seg_size;
        other.segments_ = nullptr;
        other.seg_count = 0;
        other.seg_cap = 0;
        other.first_seg = 0;
        other.first_off = 0;
        other.len = 0;
        return *this;
    }

    ~deque() {
        destroy_all();
    }

    T& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len);
        const ptrdiff_t abs = first_off + i;
        return segments_[first_seg + abs / seg_size][abs % seg_size];
    }
    const T& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len);
        const ptrdiff_t abs = first_off + i;
        return segments_[first_seg + abs / seg_size][abs % seg_size];
    }

    T& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < len && "deque::at out of range");
        return (*this)[i];
    }
    const T& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < len && "deque::at out of range");
        return (*this)[i];
    }

    T& front() noexcept { assert(len > 0); return (*this)[0]; }
    const T& front() const noexcept { assert(len > 0); return (*this)[0]; }

    T& back() noexcept { assert(len > 0); return (*this)[len - 1]; }
    const T& back() const noexcept { assert(len > 0); return (*this)[len - 1]; }

    size_t size() const noexcept { return static_cast<size_t>(len); }
    ptrdiff_t ssize() const noexcept { return len; }
    ptrdiff_t scapacity() const noexcept { return seg_count * seg_size; }
    size_t capacity() const noexcept {
        return static_cast<size_t>(seg_count) * static_cast<size_t>(seg_size);
    }
    bool empty() const noexcept { return len == 0; }

    void push_back(const T& v) {
        ensure_back_room();
        const ptrdiff_t abs = first_off + len;
        new (segments_[first_seg + abs / seg_size] + abs % seg_size) T(v);
        ++len;
    }

    void push_back(T&& v) {
        ensure_back_room();
        const ptrdiff_t abs = first_off + len;
        new (segments_[first_seg + abs / seg_size] + abs % seg_size) T(std::move(v));
        ++len;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        ensure_back_room();
        const ptrdiff_t abs = first_off + len;
        T* slot = segments_[first_seg + abs / seg_size] + abs % seg_size;
        new (slot) T(std::forward<Args>(args)...);
        ++len;
        return *slot;
    }

    void push_front(const T& v) {
        ensure_front_room();
        first_off -= 1;
        if (first_off < 0) {
            first_off = seg_size - 1;
            first_seg -= 1;
        }
        new (segments_[first_seg] + first_off) T(v);
        ++len;
    }

    void push_front(T&& v) {
        ensure_front_room();
        first_off -= 1;
        if (first_off < 0) {
            first_off = seg_size - 1;
            first_seg -= 1;
        }
        new (segments_[first_seg] + first_off) T(std::move(v));
        ++len;
    }

    template <typename... Args>
    T& emplace_front(Args&&... args) {
        ensure_front_room();
        first_off -= 1;
        if (first_off < 0) {
            first_off = seg_size - 1;
            first_seg -= 1;
        }
        T* slot = segments_[first_seg] + first_off;
        new (slot) T(std::forward<Args>(args)...);
        ++len;
        return *slot;
    }

    void pop_back() noexcept {
        assert(len > 0);
        --len;
        const ptrdiff_t abs = first_off + len;
        segments_[first_seg + abs / seg_size][abs % seg_size].~T();
    }

    void pop_front() noexcept {
        assert(len > 0);
        segments_[first_seg][first_off].~T();
        ++first_off;
        if (first_off == seg_size) {
            first_off = 0;
            first_seg += 1;
        }
        --len;
    }

    void clear() noexcept {
        while (len > 0) {
            pop_front();
        }
    }

    void shrink_to_fit() {
        // std::deque does not shrink its block array; neither do we.
        // Kept as an API surface for std::deque compatibility.
    }

    void swap(deque& other) noexcept {
        std::swap(segments_,  other.segments_);
        std::swap(seg_count,  other.seg_count);
        std::swap(seg_cap,    other.seg_cap);
        std::swap(first_seg,  other.first_seg);
        std::swap(first_off,  other.first_off);
        std::swap(len,        other.len);
        std::swap(seg_size,   other.seg_size);
    }

private:
    T** segments_;
    ptrdiff_t seg_count;
    ptrdiff_t seg_cap;
    ptrdiff_t first_seg;
    ptrdiff_t first_off;
    ptrdiff_t len;
    ptrdiff_t seg_size;

    static T* alloc_segment(ptrdiff_t seg_size) {
        void* raw = df::allocate(static_cast<size_t>(seg_size) * sizeof(T));
        assert(raw != nullptr && "deque segment allocation failed");
        return static_cast<T*>(raw);
    }

    static void free_segment(T* seg, ptrdiff_t seg_size) {
        df::deallocate(seg, static_cast<size_t>(seg_size) * sizeof(T));
    }

    void ensure_segment_array_room() {
        if (seg_count < seg_cap) {
            return;
        }
        const ptrdiff_t new_cap = seg_cap ? seg_cap + seg_cap / 2 + 1 : 8;
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(T*));
        assert(raw != nullptr && "deque segment array allocation failed");
        T** fresh = static_cast<T**>(raw);
        for (ptrdiff_t i = 0; i < seg_count; ++i) {
            fresh[i] = segments_[i];
        }
        if (segments_ != nullptr) {
            df::deallocate(segments_, static_cast<size_t>(seg_cap) * sizeof(T*));
        }
        segments_ = fresh;
        seg_cap = new_cap;
    }

    void ensure_back_room() {
        const ptrdiff_t abs = first_off + len;
        const ptrdiff_t need_seg = first_seg + abs / seg_size + 1;
        while (seg_count < need_seg) {
            ensure_segment_array_room();
            segments_[seg_count] = alloc_segment(seg_size);
            ++seg_count;
        }
    }

    void ensure_front_room() {
        if (first_seg > 0) {
            return;
        }
        if (first_off > 0) {
            return;
        }
        const ptrdiff_t new_cap = seg_cap ? seg_cap + seg_cap / 2 + 1 : 8;
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(T*));
        assert(raw != nullptr && "deque segment array allocation failed");
        T** fresh = static_cast<T**>(raw);
        const ptrdiff_t shift = new_cap - seg_count - 1;
        for (ptrdiff_t i = 0; i < seg_count; ++i) {
            fresh[shift + i] = segments_[i];
        }
        if (segments_ != nullptr) {
            df::deallocate(segments_, static_cast<size_t>(seg_cap) * sizeof(T*));
        }
        segments_ = fresh;
        seg_cap = new_cap;
        first_seg = shift;
        segments_[first_seg - 1] = alloc_segment(seg_size);
        first_seg -= 1;
        seg_count += 1;
    }

    void destroy_all() noexcept {
        clear();
        for (ptrdiff_t i = 0; i < seg_count; ++i) {
            free_segment(segments_[i], seg_size);
        }
        if (segments_ != nullptr) {
            df::deallocate(segments_, static_cast<size_t>(seg_cap) * sizeof(T*));
            segments_ = nullptr;
        }
        seg_count = 0;
        seg_cap = 0;
        first_seg = 0;
        first_off = 0;
    }
};

template <typename T>
inline void swap(deque<T>& a, deque<T>& b) noexcept {
    a.swap(b);
}

} // namespace df