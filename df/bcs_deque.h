#pragma once

// bcs_deque.h
//
// Malloc-backed growable deque with std::deque-shaped API and random-access
// iterators that walk segments natively (no contiguity assumption). Segments
// of default_seg_size elements each; segments and the segment-pointer array
// are allocated via aligned_alloc_raw / aligned_free_raw (bc_raw_alloc.h).
// Usable from any thread, any context, no set_active_pool required.
//
// Namespace: sdf = "standard df" -- malloc-backed containers, usable
// outside engine pool setup.
//
// Iterator correctness:
//   The iterator holds (segment_storage*, logical_idx). Dereference walks
//   the segment table on every access, so it is correct across segment
//   boundaries and across segment-array reallocations. It does NOT hold a
//   T* into a segment, so it is not invalidated by segment growth. Iterator
//   invalidation follows std::deque's rules:
//     - invalidated on erase() of the pointed-to element (or any erase
//       before it that shifts the live range);
//     - invalidated on insert() before the pointed-to element;
//     - NOT invalidated by push_back / push_front / pop_back / pop_front.
//
// Included API (std::deque-shaped):
//   push_back, push_front, emplace_back, emplace_front, pop_back, pop_front,
//   at, operator[], front, back,
//   insert (pos, v), insert (pos, n, v), insert (pos, first, last),
//   emplace (pos, args...), erase (pos), erase (first, last),
//   size, empty, clear, shrink_to_fit, swap,
//   begin/end/cbegin/cend (random access).
//
// Not included (deliberately, to keep compile time down):
//   get_allocator, resize, assign.
//
// Pool-backed counterpart: df::deque<T> in bc_deque.h.
//
// Style: snake_case, One True Brace Style, template param T.

#include "bc_raw_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <new>
#include <utility>

namespace sdf {

template <typename T>
struct deque_storage {
    T**       segments   = nullptr;
    ptrdiff_t seg_count  = 0;
    ptrdiff_t seg_cap    = 0;
    ptrdiff_t first_seg  = 0;
    ptrdiff_t first_off  = 0;
    ptrdiff_t len        = 0;
    ptrdiff_t seg_size   = 64;

    T* element_ptr(ptrdiff_t logical) const noexcept {
        if (segments == nullptr) {
            return nullptr;
        }
        const ptrdiff_t abs = first_off + logical;
        return segments[first_seg + abs / seg_size] + abs % seg_size;
    }
};

template <typename T>
class deque {
public:
    using value_type      = T;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;

    static constexpr ptrdiff_t default_seg_size = 64;
    static constexpr size_t    block_align      = 64;

private:
    using storage = deque_storage<T>;

    storage s_{};

public:
    // ---- iterators -------------------------------------------------------

    template <bool IsConst>
    class iter_t {
        using storage_ptr = std::conditional_t<IsConst, const storage*, storage*>;
        using value_ref   = std::conditional_t<IsConst, const T&, T&>;
        using value_pp    = std::conditional_t<IsConst, const T*, T*>;

        storage_ptr s_   = nullptr;
        ptrdiff_t   idx_ = 0;

    public:
        using value_type        = T;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::random_access_iterator_tag;

        iter_t() noexcept = default;

        iter_t(storage_ptr s, ptrdiff_t idx) noexcept
            : s_(s), idx_(idx) {}

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept
            : s_(other.s_), idx_(other.idx_) {}

        reference operator*()  const noexcept { return *s_->element_ptr(idx_); }
        pointer   operator->() const noexcept { return s_->element_ptr(idx_); }

        iter_t& operator++() noexcept { ++idx_; return *this; }
        iter_t  operator++(int) noexcept { iter_t t(*this); ++(*this); return t; }
        iter_t& operator--() noexcept { --idx_; return *this; }
        iter_t  operator--(int) noexcept { iter_t t(*this); --(*this); return t; }

        iter_t& operator+=(ptrdiff_t n) noexcept { idx_ += n; return *this; }
        iter_t& operator-=(ptrdiff_t n) noexcept { idx_ -= n; return *this; }
        iter_t  operator+(ptrdiff_t n) const noexcept { iter_t t(*this); t += n; return t; }
        iter_t  operator-(ptrdiff_t n) const noexcept { iter_t t(*this); t -= n; return t; }

        ptrdiff_t operator-(const iter_t& o) const noexcept { return idx_ - o.idx_; }

        reference operator[](ptrdiff_t n) const noexcept {
            return *s_->element_ptr(idx_ + n);
        }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return idx_ == o.idx_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return idx_ != o.idx_; }
        template <bool OtherConst>
        bool operator<(iter_t<OtherConst> const& o) const noexcept { return idx_ < o.idx_; }
        template <bool OtherConst>
        bool operator>(iter_t<OtherConst> const& o) const noexcept { return idx_ > o.idx_; }
        template <bool OtherConst>
        bool operator<=(iter_t<OtherConst> const& o) const noexcept { return idx_ <= o.idx_; }
        template <bool OtherConst>
        bool operator>=(iter_t<OtherConst> const& o) const noexcept { return idx_ >= o.idx_; }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    iterator       begin()       noexcept { return iterator(&s_, 0); }
    iterator       end()         noexcept { return iterator(&s_, s_.len); }
    const_iterator begin() const noexcept { return const_iterator(&s_, 0); }
    const_iterator end()   const noexcept { return const_iterator(&s_, s_.len); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    // ---- lifecycle -------------------------------------------------------

    explicit deque(ptrdiff_t seg_size = default_seg_size)
        : s_{} {
        s_.seg_size = seg_size > 0 ? seg_size : 1;
    }

    deque(const deque&) = delete;
    deque& operator=(const deque&) = delete;

    deque(deque&& other) noexcept
        : s_(other.s_) {
        other.s_ = storage{};
        other.s_.seg_size = default_seg_size;
    }

    deque& operator=(deque&& other) noexcept {
        if (this != &other) {
            destroy_all();
            s_ = other.s_;
            other.s_ = storage{};
            other.s_.seg_size = default_seg_size;
        }
        return *this;
    }

    ~deque() {
        destroy_all();
    }

    // ---- capacity --------------------------------------------------------

    size_t    size()  const noexcept { return static_cast<size_t>(s_.len); }
    ptrdiff_t ssize() const noexcept { return s_.len; }
    bool      empty() const noexcept { return s_.len == 0; }

    size_t capacity() const noexcept {
        return static_cast<size_t>(s_.seg_count * s_.seg_size);
    }
    ptrdiff_t scapacity() const noexcept { return s_.seg_count * s_.seg_size; }

    // ---- element access --------------------------------------------------

    T& operator[](ptrdiff_t i) noexcept {
        assert(i >= 0 && i < s_.len);
        return *s_.element_ptr(i);
    }
    const T& operator[](ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < s_.len);
        return *s_.element_ptr(i);
    }

    T& at(ptrdiff_t i) noexcept {
        assert(i >= 0 && i < s_.len && "deque::at out of range");
        return *s_.element_ptr(i);
    }
    const T& at(ptrdiff_t i) const noexcept {
        assert(i >= 0 && i < s_.len && "deque::at out of range");
        return *s_.element_ptr(i);
    }

    T& front() noexcept { assert(s_.len > 0); return *s_.element_ptr(0); }
    const T& front() const noexcept { assert(s_.len > 0); return *s_.element_ptr(0); }

    T& back() noexcept { assert(s_.len > 0); return *s_.element_ptr(s_.len - 1); }
    const T& back() const noexcept { assert(s_.len > 0); return *s_.element_ptr(s_.len - 1); }

    // ---- modifiers -------------------------------------------------------

    void push_back(const T& v) {
        ensure_back_room();
        const ptrdiff_t abs = s_.first_off + s_.len;
        new (s_.segments[s_.first_seg + abs / s_.seg_size] + abs % s_.seg_size) T(v);
        ++s_.len;
    }

    void push_back(T&& v) {
        ensure_back_room();
        const ptrdiff_t abs = s_.first_off + s_.len;
        new (s_.segments[s_.first_seg + abs / s_.seg_size] + abs % s_.seg_size) T(std::move(v));
        ++s_.len;
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        ensure_back_room();
        const ptrdiff_t abs = s_.first_off + s_.len;
        T* slot = s_.segments[s_.first_seg + abs / s_.seg_size] + abs % s_.seg_size;
        new (slot) T(std::forward<Args>(args)...);
        ++s_.len;
        return *slot;
    }

    void push_front(const T& v) {
        ensure_front_room();
        --s_.first_off;
        if (s_.first_off < 0) {
            s_.first_off = s_.seg_size - 1;
            --s_.first_seg;
        }
        new (s_.segments[s_.first_seg] + s_.first_off) T(v);
        ++s_.len;
    }

    void push_front(T&& v) {
        ensure_front_room();
        --s_.first_off;
        if (s_.first_off < 0) {
            s_.first_off = s_.seg_size - 1;
            --s_.first_seg;
        }
        new (s_.segments[s_.first_seg] + s_.first_off) T(std::move(v));
        ++s_.len;
    }

    template <typename... Args>
    T& emplace_front(Args&&... args) {
        ensure_front_room();
        --s_.first_off;
        if (s_.first_off < 0) {
            s_.first_off = s_.seg_size - 1;
            --s_.first_seg;
        }
        T* slot = s_.segments[s_.first_seg] + s_.first_off;
        new (slot) T(std::forward<Args>(args)...);
        ++s_.len;
        return *slot;
    }

    void pop_back() noexcept {
        assert(s_.len > 0);
        --s_.len;
        s_.element_ptr(s_.len)->~T();
    }

    void pop_front() noexcept {
        assert(s_.len > 0);
        s_.element_ptr(0)->~T();
        ++s_.first_off;
        if (s_.first_off == s_.seg_size) {
            s_.first_off = 0;
            ++s_.first_seg;
        }
        --s_.len;
    }

    iterator insert(const_iterator pos, const T& v) {
        T staged(v);
        return insert_impl(pos.idx_, std::move(staged));
    }
    iterator insert(const_iterator pos, T&& v) {
        return insert_impl(pos.idx_, std::move(v));
    }

    iterator insert(const_iterator pos, ptrdiff_t n, const T& v) {
        if (n <= 0) {
            return iterator(&s_, pos.idx_);
        }
        ptrdiff_t i = pos.idx_;
        for (ptrdiff_t k = 0; k < n; ++k) {
            T staged(v);
            insert_impl(i, std::move(staged));
            ++i;
        }
        return iterator(&s_, pos.idx_);
    }

    template <typename InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last) {
        ptrdiff_t i = pos.idx_;
        for (; first != last; ++first) {
            T staged(*first);
            insert_impl(i, std::move(staged));
            ++i;
        }
        return iterator(&s_, pos.idx_);
    }

    template <typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        T staged(std::forward<Args>(args)...);
        return insert_impl(pos.idx_, std::move(staged));
    }

    iterator erase(const_iterator pos) {
        const ptrdiff_t i = pos.idx_;
        assert(i >= 0 && i < s_.len);
        erase_logical(i);
        return iterator(&s_, i);
    }

    iterator erase(const_iterator first, const_iterator last) {
        const ptrdiff_t f = first.idx_;
        const ptrdiff_t l = last.idx_;
        if (f >= l) {
            return iterator(&s_, f);
        }
        for (ptrdiff_t i = l - 1; i >= f; --i) {
            erase_logical(i);
        }
        return iterator(&s_, f);
    }

    void clear() noexcept {
        while (s_.len > 0) {
            pop_front();
        }
    }

    void shrink_to_fit() {
        // std::deque does not shrink its block array; neither do we.
        // Kept as an API surface for std::deque compatibility.
    }

    void swap(deque& other) noexcept {
        std::swap(s_, other.s_);
    }

private:
    static T* alloc_segment(ptrdiff_t seg_size) {
        void* raw = aligned_alloc_raw(static_cast<size_t>(seg_size) * sizeof(T), block_align);
        assert(raw != nullptr && "sdf::deque: segment allocation failed");
        return static_cast<T*>(raw);
    }

    static void free_segment(T* seg) {
        aligned_free_raw(seg);
    }

    void ensure_segment_array_room() {
        if (s_.seg_count < s_.seg_cap) {
            return;
        }
        ptrdiff_t new_cap = s_.seg_cap ? s_.seg_cap + s_.seg_cap / 2 + 1 : 8;
        void* raw = aligned_alloc_raw(static_cast<size_t>(new_cap) * sizeof(T*), block_align);
        assert(raw != nullptr && "sdf::deque: segment array allocation failed");
        T** fresh = static_cast<T**>(raw);
        for (ptrdiff_t i = 0; i < s_.seg_count; ++i) {
            fresh[i] = s_.segments[i];
        }
        if (s_.segments != nullptr) {
            aligned_free_raw(s_.segments);
        }
        s_.segments = fresh;
        s_.seg_cap = new_cap;
    }

    void ensure_back_room() {
        const ptrdiff_t abs = s_.first_off + s_.len;
        const ptrdiff_t need_seg = s_.first_seg + abs / s_.seg_size + 1;
        while (s_.seg_count < need_seg) {
            ensure_segment_array_room();
            s_.segments[s_.seg_count] = alloc_segment(s_.seg_size);
            ++s_.seg_count;
        }
    }

    void ensure_front_room() {
        if (s_.first_seg > 0) {
            return;
        }
        if (s_.first_off > 0) {
            return;
        }
        ptrdiff_t new_cap = s_.seg_cap ? s_.seg_cap + s_.seg_cap / 2 + 1 : 8;
        void* raw = aligned_alloc_raw(static_cast<size_t>(new_cap) * sizeof(T*), block_align);
        assert(raw != nullptr && "sdf::deque: segment array allocation failed");
        T** fresh = static_cast<T**>(raw);
        const ptrdiff_t shift = new_cap - s_.seg_count - 1;
        for (ptrdiff_t i = 0; i < s_.seg_count; ++i) {
            fresh[shift + i] = s_.segments[i];
        }
        if (s_.segments != nullptr) {
            aligned_free_raw(s_.segments);
        }
        s_.segments = fresh;
        s_.seg_cap = new_cap;
        s_.first_seg = shift;
        s_.segments[s_.first_seg - 1] = alloc_segment(s_.seg_size);
        --s_.first_seg;
        ++s_.seg_count;
    }

    void destroy_all() noexcept {
        clear();
        for (ptrdiff_t i = 0; i < s_.seg_count; ++i) {
            free_segment(s_.segments[i]);
        }
        if (s_.segments != nullptr) {
            aligned_free_raw(s_.segments);
        }
        s_ = storage{};
        s_.seg_size = default_seg_size;
    }

    iterator insert_impl(ptrdiff_t i, T&& v) {
        assert(i >= 0 && i <= s_.len);
        if (i == s_.len) {
            push_back(std::move(v));
            return iterator(&s_, i);
        }
        if (i == 0) {
            push_front(std::move(v));
            return iterator(&s_, 0);
        }
        if (i <= s_.len / 2) {
            push_front(std::move(front()));
            for (ptrdiff_t j = 0; j < i; ++j) {
                *s_.element_ptr(j) = std::move(*s_.element_ptr(j + 1));
            }
            *s_.element_ptr(i) = std::move(v);
        } else {
            push_back(std::move(back()));
            for (ptrdiff_t j = s_.len - 1; j > i; --j) {
                *s_.element_ptr(j) = std::move(*s_.element_ptr(j - 1));
            }
            *s_.element_ptr(i) = std::move(v);
        }
        return iterator(&s_, i);
    }

    void erase_logical(ptrdiff_t i) {
        assert(i >= 0 && i < s_.len);
        if (i == 0) {
            pop_front();
        } else if (i == s_.len - 1) {
            pop_back();
        } else if (i <= s_.len / 2) {
            for (ptrdiff_t j = i; j > 0; --j) {
                *s_.element_ptr(j) = std::move(*s_.element_ptr(j - 1));
            }
            pop_front();
        } else {
            for (ptrdiff_t j = i; j < s_.len - 1; ++j) {
                *s_.element_ptr(j) = std::move(*s_.element_ptr(j + 1));
            }
            pop_back();
        }
    }
};

} // namespace sdf