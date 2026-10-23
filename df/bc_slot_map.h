#pragma once

// bc_slot_map.h
//
// Generational slot map: gives out stable df::handle<Tag> values that become
// detectably stale when the underlying slot is reused. Common uses:
//   - entity IDs (handle<Entity>) storing entity metadata (slot_map<EntityRecord, Entity>)
//   - resource IDs (handle<Texture>, handle<Mesh>, ...)
//   - any subsystem that gives out references that could outlive the
//     underlying object
//
// The stored value type T and the handle tag type Tag are independent.
// handle<Tag> is a strong typedef over (index, generation); it carries no
// value_type relationship. Use the default (Tag = T) when the handle tag and
// the stored type are the same; supply a distinct Tag when they are not
// (e.g. slot_map<entity_record, uint32_t> hands out handle<uint32_t>).
//
// Model:
//   slots_           dense array of { generation, live, T, next_free }
//   free_head_       freelist of freed slot indices
//   dense_to_slot_   dense array of slot indices for live slots
//   slot_to_dense_   reverse map: slot index -> position in dense_to_slot_
//
// Stale handles: after erase(h), the slot's generation increments. Old h
// has the old generation, so get(h) returns nullptr. Never dangling.
//
// Style: snake_case, One True Brace Style, template params T, Tag.

#include "bc_handle.h"
#include "bc_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

template <typename T, typename Tag = T>
class slot_map {
public:
    using value_type      = T;
    using handle_type     = handle<Tag>;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;

    static constexpr uint32_t invalid_slot = 0xFFFFFFFFu;

private:
    struct slot {
        uint32_t generation;
        bool     live;
        T        value;
        uint32_t next_free;
    };

    slot*     slots_        = nullptr;
    ptrdiff_t slot_count_   = 0;
    ptrdiff_t slot_cap_     = 0;
    ptrdiff_t live_count_   = 0;

    ptrdiff_t* dense_to_slot_ = nullptr;
    ptrdiff_t  dense_cap_     = 0;

    ptrdiff_t* slot_to_dense_ = nullptr;
    ptrdiff_t  slot_to_dense_cap_ = 0;

    uint32_t   free_head_    = invalid_slot;

public:
    template <bool IsConst>
    class iter_t {
        using slot_ptr  = std::conditional_t<IsConst, const slot*, slot*>;
        using ptr_array = std::conditional_t<IsConst, const ptrdiff_t*, ptrdiff_t*>;
        using value_ref = std::conditional_t<IsConst, const T&, T&>;
        using value_pp  = std::conditional_t<IsConst, const T*, T*>;

        slot_ptr  slots_ = nullptr;
        ptr_array dense_ = nullptr;
        ptrdiff_t pos_   = 0;
        ptrdiff_t end_   = 0;

    public:
        using value_type        = T;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::forward_iterator_tag;

        iter_t() noexcept = default;

        iter_t(slot_ptr slots, ptr_array dense, ptrdiff_t pos, ptrdiff_t end) noexcept
            : slots_(slots), dense_(dense), pos_(pos), end_(end) {}

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept
            : slots_(other.slots_), dense_(other.dense_), pos_(other.pos_), end_(other.end_) {}

        reference operator*()  const noexcept { return slots_[dense_[pos_]].value; }
        pointer   operator->() const noexcept { return &slots_[dense_[pos_]].value; }

        iter_t& operator++() noexcept { ++pos_; return *this; }
        iter_t  operator++(int) noexcept { iter_t t(*this); ++(*this); return t; }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return pos_ == o.pos_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return pos_ != o.pos_; }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    iterator       begin()       noexcept { return iterator(slots_, dense_to_slot_, 0, live_count_); }
    iterator       end()         noexcept { return iterator(slots_, dense_to_slot_, live_count_, live_count_); }
    const_iterator begin() const noexcept { return const_iterator(slots_, dense_to_slot_, 0, live_count_); }
    const_iterator end()   const noexcept { return const_iterator(slots_, dense_to_slot_, live_count_, live_count_); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    slot_map() noexcept {}

    slot_map(const slot_map&) = delete;
    slot_map& operator=(const slot_map&) = delete;

    slot_map(slot_map&& other) noexcept {
        steal(other);
    }

    slot_map& operator=(slot_map&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~slot_map() {
        destroy_all();
    }

    size_t    size()          const noexcept { return static_cast<size_t>(live_count_); }
    ptrdiff_t ssize()         const noexcept { return live_count_; }
    bool      empty()         const noexcept { return live_count_ == 0; }
    ptrdiff_t slot_capacity() const noexcept { return slot_cap_; }
    ptrdiff_t live_capacity() const noexcept { return dense_cap_; }

    void reserve(ptrdiff_t n) {
        if (n > slot_cap_) {
            grow_slots(n);
        }
        if (n > dense_cap_) {
            grow_dense(n);
        }
    }

    handle_type insert(const T& v) {
        return do_insert(v);
    }
    handle_type insert(T&& v) {
        return do_insert(std::move(v));
    }

    template <typename... Args>
    handle_type emplace(Args&&... args) {
        const uint32_t idx = acquire_slot();
        slot& s = slots_[idx];
        new (&s.value) T(std::forward<Args>(args)...);
        s.live = true;
        ++live_count_;
        append_dense(static_cast<ptrdiff_t>(idx));
        return handle_type(idx, s.generation);
    }

    bool erase(handle_type h) {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(h.index);
        if (idx < 0 || idx >= slot_count_) {
            return false;
        }
        slot& s = slots_[idx];
        if (!s.live || s.generation != h.generation) {
            return false;
        }
        s.value.~T();
        s.live = false;
        s.generation += 1;
        if (s.generation == 0) {
            s.generation = 1;
        }
        s.next_free = free_head_;
        free_head_ = static_cast<uint32_t>(idx);
        --live_count_;
        remove_dense(idx);
        return true;
    }

    void clear() noexcept {
        for (ptrdiff_t i = 0; i < slot_count_; ++i) {
            if (slots_[i].live) {
                slots_[i].value.~T();
                slots_[i].live = false;
            }
        }
        live_count_ = 0;
        free_head_ = invalid_slot;
        for (ptrdiff_t i = slot_count_ - 1; i >= 0; --i) {
            slots_[i].next_free = free_head_;
            free_head_ = static_cast<uint32_t>(i);
        }
        for (ptrdiff_t i = 0; i < slot_to_dense_cap_; ++i) {
            slot_to_dense_[i] = -1;
        }
    }

    void swap(slot_map& other) noexcept {
        std::swap(slots_, other.slots_);
        std::swap(slot_count_, other.slot_count_);
        std::swap(slot_cap_, other.slot_cap_);
        std::swap(live_count_, other.live_count_);
        std::swap(dense_to_slot_, other.dense_to_slot_);
        std::swap(dense_cap_, other.dense_cap_);
        std::swap(slot_to_dense_, other.slot_to_dense_);
        std::swap(slot_to_dense_cap_, other.slot_to_dense_cap_);
        std::swap(free_head_, other.free_head_);
    }

    T* get(handle_type h) noexcept {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(h.index);
        if (idx < 0 || idx >= slot_count_) {
            return nullptr;
        }
        slot& s = slots_[idx];
        if (!s.live || s.generation != h.generation) {
            return nullptr;
        }
        return &s.value;
    }
    const T* get(handle_type h) const noexcept {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(h.index);
        if (idx < 0 || idx >= slot_count_) {
            return nullptr;
        }
        const slot& s = slots_[idx];
        if (!s.live || s.generation != h.generation) {
            return nullptr;
        }
        return &s.value;
    }

    bool contains(handle_type h) const noexcept {
        return get(h) != nullptr;
    }

private:
    void append_dense(ptrdiff_t slot_idx) {
        if (live_count_ == dense_cap_) {
            grow_dense(dense_cap_ > 0 ? dense_cap_ + dense_cap_ / 2 + 8 : 16);
        }
        if (slot_idx >= slot_to_dense_cap_) {
            grow_slot_to_dense(slot_idx + 1);
        }
        dense_to_slot_[live_count_] = slot_idx;
        slot_to_dense_[slot_idx] = live_count_;
    }

    void remove_dense(ptrdiff_t slot_idx) {
        const ptrdiff_t dense_pos = slot_to_dense_[slot_idx];
        const ptrdiff_t last_pos = live_count_ - 1;
        if (dense_pos != last_pos) {
            const ptrdiff_t moved_slot = dense_to_slot_[last_pos];
            dense_to_slot_[dense_pos] = moved_slot;
            slot_to_dense_[moved_slot] = dense_pos;
        }
        dense_to_slot_[last_pos] = 0;
        slot_to_dense_[slot_idx] = -1;
    }

    uint32_t acquire_slot() {
        if (free_head_ != invalid_slot) {
            const uint32_t idx = free_head_;
            free_head_ = slots_[idx].next_free;
            return idx;
        }
        if (slot_count_ == slot_cap_) {
            grow_slots(slot_cap_ > 0 ? slot_cap_ + slot_cap_ / 2 + 8 : 16);
        }
        const ptrdiff_t idx = slot_count_;
        ++slot_count_;
        slot& s = slots_[idx];
        s.generation = 1;
        s.live = false;
        s.next_free = invalid_slot;
        if (idx >= slot_to_dense_cap_) {
            grow_slot_to_dense(idx + 1);
        }
        slot_to_dense_[idx] = -1;
        return static_cast<uint32_t>(idx);
    }

    template <typename V>
    handle_type do_insert(V&& v) {
        const uint32_t idx = acquire_slot();
        slot& s = slots_[idx];
        new (&s.value) T(std::forward<V>(v));
        s.live = true;
        ++live_count_;
        append_dense(static_cast<ptrdiff_t>(idx));
        return handle_type(idx, s.generation);
    }

    void grow_slots(ptrdiff_t new_cap) {
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(slot));
        assert(raw != nullptr && "slot_map: slots allocation failed");
        slot* fresh = static_cast<slot*>(raw);
        if (slot_count_ > 0) {
            memcpy(fresh, slots_, static_cast<size_t>(slot_count_) * sizeof(slot));
        }
        if (slots_ != nullptr) {
            df::deallocate(slots_, static_cast<size_t>(slot_cap_) * sizeof(slot));
        }
        slots_ = fresh;
        slot_cap_ = new_cap;
    }

    void grow_dense(ptrdiff_t new_cap) {
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(ptrdiff_t));
        assert(raw != nullptr && "slot_map: dense allocation failed");
        ptrdiff_t* fresh = static_cast<ptrdiff_t*>(raw);
        if (live_count_ > 0) {
            memcpy(fresh, dense_to_slot_, static_cast<size_t>(live_count_) * sizeof(ptrdiff_t));
        }
        if (dense_to_slot_ != nullptr) {
            df::deallocate(dense_to_slot_, static_cast<size_t>(dense_cap_) * sizeof(ptrdiff_t));
        }
        dense_to_slot_ = fresh;
        dense_cap_ = new_cap;
    }

    void grow_slot_to_dense(ptrdiff_t new_cap) {
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(ptrdiff_t));
        assert(raw != nullptr && "slot_map: reverse map allocation failed");
        ptrdiff_t* fresh = static_cast<ptrdiff_t*>(raw);
        for (ptrdiff_t i = 0; i < new_cap; ++i) {
            fresh[i] = -1;
        }
        if (slot_to_dense_cap_ > 0) {
            memcpy(fresh, slot_to_dense_,
                   static_cast<size_t>(slot_to_dense_cap_) * sizeof(ptrdiff_t));
        }
        if (slot_to_dense_ != nullptr) {
            df::deallocate(slot_to_dense_, static_cast<size_t>(slot_to_dense_cap_) * sizeof(ptrdiff_t));
        }
        slot_to_dense_ = fresh;
        slot_to_dense_cap_ = new_cap;
    }

    void destroy_all() noexcept {
        if (slots_ != nullptr) {
            for (ptrdiff_t i = 0; i < slot_count_; ++i) {
                if (slots_[i].live) {
                    slots_[i].value.~T();
                }
            }
            df::deallocate(slots_, static_cast<size_t>(slot_cap_) * sizeof(slot));
            slots_ = nullptr;
        }
        if (dense_to_slot_ != nullptr) {
            df::deallocate(dense_to_slot_, static_cast<size_t>(dense_cap_) * sizeof(ptrdiff_t));
            dense_to_slot_ = nullptr;
        }
        if (slot_to_dense_ != nullptr) {
            df::deallocate(slot_to_dense_, static_cast<size_t>(slot_to_dense_cap_) * sizeof(ptrdiff_t));
            slot_to_dense_ = nullptr;
        }
        slot_count_ = 0;
        slot_cap_ = 0;
        live_count_ = 0;
        dense_cap_ = 0;
        slot_to_dense_cap_ = 0;
        free_head_ = invalid_slot;
    }

    void steal(slot_map& other) noexcept {
        slots_ = other.slots_;
        slot_count_ = other.slot_count_;
        slot_cap_ = other.slot_cap_;
        live_count_ = other.live_count_;
        dense_to_slot_ = other.dense_to_slot_;
        dense_cap_ = other.dense_cap_;
        slot_to_dense_ = other.slot_to_dense_;
        slot_to_dense_cap_ = other.slot_to_dense_cap_;
        free_head_ = other.free_head_;

        other.slots_ = nullptr;
        other.slot_count_ = 0;
        other.slot_cap_ = 0;
        other.live_count_ = 0;
        other.dense_to_slot_ = nullptr;
        other.dense_cap_ = 0;
        other.slot_to_dense_ = nullptr;
        other.slot_to_dense_cap_ = 0;
        other.free_head_ = invalid_slot;
    }
};

template <typename T, typename Tag>
inline void swap(slot_map<T, Tag>& a, slot_map<T, Tag>& b) noexcept {
    a.swap(b);
}

} // namespace df