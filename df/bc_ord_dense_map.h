#pragma once

// bc_ord_dense_map.h
//
// Insertion-ordered hash map, built on df::unord_dense_map's probe engine.
//
// ord_ = insertion-ordered. Iteration yields entries in the order they were
// inserted. This is NOT std::map's sorted-by-key order -- this is a hash map,
// there is no comparator. The only difference from unord_dense_map is
// deterministic iteration order.
//
// Purpose: config files, save files, anything where the write-back order is
// the insertion order. Not a general-purpose map -- for that, use
// unord_dense_map directly.
//
// IMPORTANT (map only): keys are mutable in the stored pair
// (pair<K, V>, not pair<const K, V>). Same footgun as unord_dense_map.
// The SET specialization (V == void) yields const K& and has no footgun.
//
// If you need const-key semantics on a map, use df::unordered_map (chained)
// or df::map (red-black).
//
// Design:
//   - index_ : unord_dense_map<K, uint32_t>  (key -> position in slots_)
//   - slots_ : raw df::allocate'd array of value_type (pair<K,V> for map,
//              K for set)
//   - live_  : raw df::allocate'd uint8_t array, 1 per live slot, 0 per
//              tombstone
//   - entries_count_ : high water mark in slots_ (live + tombstones)
//   - live_count_    : number of live entries
//   - on erase: call the value's destructor, mark live_[vi] = 0, erase from
//     index. Owned resources (string bodies, etc.) ARE freed on erase.
//   - on insert: placement-new into slots_[entries_count_], live_[i] = 1
//   - iteration skips tombstones
//   - compaction moves live entries forward, destroys moved-from slots,
//     rebuilds the index. Auto-compacts when tombstones > live_count_.
//
// count() returns size_t, matching std::map-style count semantics (0 or 1).
//
// Pool-backed via df::allocate / df::deallocate.
//
// Malloc-backed counterpart: sdf::ord_dense_map<K, V> in bcs_ord_dense_map.h.
//
// Style: snake_case, One True Brace Style, template params K, V.

#include "bc_allocator.h"
#include "bc_hash.h"
#include "bc_pair.h"
#include "bc_unord_dense_map.h"

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <new>
#include <utility>
#include <string.h>
#include <type_traits>

namespace df {

namespace detail {

template <typename K, typename V>
struct ord_dense_value_traits {
    using value_type = pair<K, V>;

    template <typename KK, typename VV>
    static void construct(value_type* at, KK&& k, VV&& v) {
        new (&at->first)  K(std::forward<KK>(k));
        new (&at->second) V(std::forward<VV>(v));
    }
    static void construct_default(value_type* at, const K& k) {
        new (&at->first)  K(k);
        new (&at->second) V();
    }
    static void construct_move(value_type* at, value_type& src) {
        new (&at->first)  K(std::move(src.first));
        new (&at->second) V(std::move(src.second));
    }
    static void construct_move_noexcept(value_type* at, value_type& src) {
        new (&at->first)  K(std::move_if_noexcept(src.first));
        new (&at->second) V(std::move_if_noexcept(src.second));
    }
    static void destroy(value_type* at) noexcept {
        at->second.~V();
        at->first.~K();
    }
    static K&       key_of(value_type& v)       noexcept { return v.first; }
    static const K& key_of(const value_type& v) noexcept { return v.first; }
};

template <typename K>
struct ord_dense_value_traits<K, void> {
    using value_type = K;

    static void construct(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void construct(value_type* at, K&& k) {
        new (at) K(std::move(k));
    }
    static void construct_default(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void construct_move(value_type* at, value_type& src) {
        new (at) K(std::move(src));
    }
    static void construct_move_noexcept(value_type* at, value_type& src) {
        new (at) K(std::move_if_noexcept(src));
    }
    static void destroy(value_type* at) noexcept {
        at->~K();
    }
    static K&       key_of(value_type& v)       noexcept { return v; }
    static const K& key_of(const value_type& v) noexcept { return v; }
};

} // namespace detail

template <typename K, typename V>
class ord_dense_map {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = typename detail::ord_dense_value_traits<K, V>::value_type;
    using size_type   = size_t;

    static constexpr bool is_set_v = std::is_void_v<V>;

private:
    using index_map = unord_dense_map<K, uint32_t>;
    using traits    = detail::ord_dense_value_traits<K, V>;

    static constexpr bool trivially_relocatable =
        std::is_trivially_copyable_v<value_type> &&
        std::is_trivially_destructible_v<value_type>;

    struct place_result {
        ptrdiff_t value_idx;
        bool      inserted;
    };

    value_type*   slots_         = nullptr;
    uint8_t*      live_          = nullptr;
    ptrdiff_t     cap_           = 0;
    ptrdiff_t     entries_count_ = 0;
    ptrdiff_t     live_count_    = 0;
    index_map     index_;

public:
    // ---- iterators -------------------------------------------------------

    template <bool IsConst>
    class iter_t {
        using slot_ptr  = std::conditional_t<IsConst, const value_type*, value_type*>;
        using live_ptr  = std::conditional_t<IsConst, const uint8_t*, uint8_t*>;
        using value_ref = std::conditional_t<
            IsConst || is_set_v,
            const value_type&,
            value_type&>;
        using value_pp = std::conditional_t<
            IsConst || is_set_v,
            const value_type*,
            value_type*>;

        slot_ptr base_ = nullptr;
        live_ptr live_ = nullptr;
        ptrdiff_t pos_ = 0;
        ptrdiff_t end_ = 0;

        void skip_dead() {
            while (pos_ < end_ && !live_[pos_]) {
                ++pos_;
            }
        }

    public:
        using value_type        = ord_dense_map::value_type;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::forward_iterator_tag;

        iter_t() noexcept = default;

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept
            : base_(other.base_), live_(other.live_), pos_(other.pos_), end_(other.end_) {}

        iter_t(slot_ptr base, live_ptr live, ptrdiff_t pos, ptrdiff_t end) noexcept
            : base_(base), live_(live), pos_(pos), end_(end) {
            skip_dead();
        }

        reference operator*()  const noexcept { return base_[pos_]; }
        pointer   operator->() const noexcept { return &base_[pos_]; }

        iter_t& operator++() noexcept {
            ++pos_;
            skip_dead();
            return *this;
        }
        iter_t operator++(int) noexcept {
            iter_t t(*this);
            ++(*this);
            return t;
        }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return pos_ == o.pos_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return pos_ != o.pos_; }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    // ---- lifecycle -------------------------------------------------------

    ord_dense_map() noexcept {}

    ord_dense_map(const ord_dense_map&) = delete;
    ord_dense_map& operator=(const ord_dense_map&) = delete;

    ord_dense_map(ord_dense_map&& other) noexcept {
        steal(other);
    }

    ord_dense_map& operator=(ord_dense_map&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~ord_dense_map() {
        destroy_all();
    }

    // ---- capacity --------------------------------------------------------

    size_t    size()      const noexcept { return static_cast<size_t>(live_count_); }
    ptrdiff_t ssize()     const noexcept { return live_count_; }
    ptrdiff_t scapacity() const noexcept { return cap_; }
    size_t    capacity()  const noexcept { return static_cast<size_t>(cap_); }
    bool      empty()     const noexcept { return live_count_ == 0; }

    void reserve(ptrdiff_t n) {
        if (n <= 0) {
            return;
        }
        if (n > cap_) {
            realloc_slots(n);
        }
        index_.reserve(n);
    }

    // ---- iteration -------------------------------------------------------

    iterator begin() noexcept {
        return iterator(slots_, live_, 0, entries_count_);
    }
    iterator end() noexcept {
        return iterator(slots_, live_, entries_count_, entries_count_);
    }
    const_iterator begin() const noexcept {
        return const_iterator(slots_, live_, 0, entries_count_);
    }
    const_iterator end() const noexcept {
        return const_iterator(slots_, live_, entries_count_, entries_count_);
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (!live_[i]) {
                continue;
            }
            if constexpr (is_set_v) {
                fn(static_cast<const K&>(traits::key_of(slots_[i])));
            } else {
                fn(const_cast<const K&>(slots_[i].first), slots_[i].second);
            }
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (!live_[i]) {
                continue;
            }
            if constexpr (is_set_v) {
                fn(traits::key_of(slots_[i]));
            } else {
                fn(traits::key_of(slots_[i]), slots_[i].second);
            }
        }
    }

    // ---- lookup ----------------------------------------------------------

    value_type* find(const K& key) noexcept {
        uint32_t* p = index_.find(key);
        if (p == nullptr) {
            return nullptr;
        }
        return &slots_[*p];
    }
    const value_type* find(const K& key) const noexcept {
        const uint32_t* p = index_.find(key);
        if (p == nullptr) {
            return nullptr;
        }
        return &slots_[*p];
    }

    bool contains(const K& key) const noexcept {
        return index_.contains(key);
    }

    size_t count(const K& key) const noexcept {
        return index_.contains(key) ? 1 : 0;
    }

    V& at(const K& key) {
        static_assert(!is_set_v, "at() is not available on a set");
        uint32_t* p = index_.find(key);
        assert(p != nullptr && "ord_dense_map::at key not found");
        return slots_[*p].second;
    }
    const V& at(const K& key) const {
        static_assert(!is_set_v, "at() is not available on a set");
        const uint32_t* p = index_.find(key);
        assert(p != nullptr && "ord_dense_map::at key not found");
        return slots_[*p].second;
    }

    // ---- modifiers -------------------------------------------------------

    void insert_or_assign(const K& key, const V& v) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(key, v);
    }
    void insert_or_assign(K&& key, V&& v) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(std::move(key), std::move(v));
    }

    bool insert(const K& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        place_result r = place_key(key);
        return r.inserted;
    }
    bool insert(K&& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        place_result r = place_key(std::move(key));
        return r.inserted;
    }

    bool try_insert(const K& key, const V& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        place_result r = place_kv(key, v);
        return r.inserted;
    }
    bool try_insert(K&& key, V&& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        place_result r = place_kv(std::move(key), std::move(v));
        return r.inserted;
    }

    V& operator[](const K& key) {
        static_assert(!is_set_v, "operator[] is not available on a set");
        uint32_t* p = index_.find(key);
        if (p != nullptr) {
            return slots_[*p].second;
        }
        place_result r = place_default(key);
        return slots_[r.value_idx].second;
    }

    bool erase(const K& key) {
        uint32_t* p = index_.find(key);
        if (p == nullptr) {
            return false;
        }
        const ptrdiff_t vi = *p;
        index_.erase(key);
        destroy_slot(vi);
        --live_count_;
        maybe_compact();
        return true;
    }

    void clear() noexcept {
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (live_[i]) {
                traits::destroy(&slots_[i]);
            }
        }
        entries_count_ = 0;
        live_count_ = 0;
        index_.clear();
    }

    void rehash(ptrdiff_t new_bucket_count) {
        index_.rehash(new_bucket_count);
    }

    // ---- compaction ------------------------------------------------------

    void compact() {
        if (entries_count_ == live_count_) {
            return;
        }

        ptrdiff_t write = 0;
        for (ptrdiff_t read = 0; read < entries_count_; ++read) {
            if (!live_[read]) {
                continue;
            }
            if (write != read) {
                traits::construct_move(&slots_[write], slots_[read]);
                traits::destroy(&slots_[read]);
                live_[read] = 0;
            }
            ++write;
        }
        entries_count_ = write;
        live_count_ = write;

        index_.clear();
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            index_.insert(traits::key_of(slots_[i]), static_cast<uint32_t>(i));
        }
    }

    ptrdiff_t tombstones() const noexcept {
        return entries_count_ - live_count_;
    }

    void swap(ord_dense_map& other) noexcept {
        std::swap(slots_, other.slots_);
        std::swap(live_, other.live_);
        std::swap(cap_, other.cap_);
        std::swap(entries_count_, other.entries_count_);
        std::swap(live_count_, other.live_count_);
        index_.swap(other.index_);
    }

private:
    void construct_slot_default(ptrdiff_t i, const K& k) {
        traits::construct_default(&slots_[i], k);
        live_[i] = 1;
    }

    void destroy_slot(ptrdiff_t i) noexcept {
        traits::destroy(&slots_[i]);
        live_[i] = 0;
    }

    void free_arrays() noexcept {
        if (slots_ != nullptr) {
            df::deallocate(slots_, static_cast<size_t>(cap_) * sizeof(value_type));
            slots_ = nullptr;
        }
        if (live_ != nullptr) {
            df::deallocate(live_, static_cast<size_t>(cap_));
            live_ = nullptr;
        }
        cap_ = 0;
        entries_count_ = 0;
        live_count_ = 0;
    }

    void destroy_all() noexcept {
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (live_[i]) {
                traits::destroy(&slots_[i]);
            }
        }
        live_count_ = 0;
        entries_count_ = 0;
        free_arrays();
    }

    void steal(ord_dense_map& other) noexcept {
        slots_         = other.slots_;
        live_          = other.live_;
        cap_           = other.cap_;
        entries_count_ = other.entries_count_;
        live_count_    = other.live_count_;
        index_         = std::move(other.index_);

        other.slots_         = nullptr;
        other.live_          = nullptr;
        other.cap_           = 0;
        other.entries_count_ = 0;
        other.live_count_    = 0;
    }

    void realloc_slots_trivial(ptrdiff_t new_cap) {
        void* raw_slots = df::allocate(static_cast<size_t>(new_cap) * sizeof(value_type));
        assert(raw_slots != nullptr && "ord_dense_map: slots allocation failed");
        value_type* fresh_slots = static_cast<value_type*>(raw_slots);

        void* raw_live = df::allocate(static_cast<size_t>(new_cap));
        assert(raw_live != nullptr && "ord_dense_map: live allocation failed");
        uint8_t* fresh_live = static_cast<uint8_t*>(raw_live);
        memset(fresh_live, 0, static_cast<size_t>(new_cap));

        ptrdiff_t written = 0;
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (!live_[i]) {
                continue;
            }
            memcpy(&fresh_slots[written], &slots_[i], sizeof(value_type));
            fresh_live[written] = 1;
            ++written;
        }

        if (slots_ != nullptr) {
            df::deallocate(slots_, static_cast<size_t>(cap_) * sizeof(value_type));
        }
        if (live_ != nullptr) {
            df::deallocate(live_, static_cast<size_t>(cap_));
        }

        slots_ = fresh_slots;
        live_ = fresh_live;
        cap_ = new_cap;
        entries_count_ = written;
        live_count_ = written;

        index_.clear();
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            index_.insert(traits::key_of(slots_[i]), static_cast<uint32_t>(i));
        }
    }

    void realloc_slots_strong(ptrdiff_t new_cap) {
        void* raw_slots = df::allocate(static_cast<size_t>(new_cap) * sizeof(value_type));
        assert(raw_slots != nullptr && "ord_dense_map: slots allocation failed");
        value_type* fresh_slots = static_cast<value_type*>(raw_slots);

        void* raw_live = df::allocate(static_cast<size_t>(new_cap));
        assert(raw_live != nullptr && "ord_dense_map: live allocation failed");
        uint8_t* fresh_live = static_cast<uint8_t*>(raw_live);
        memset(fresh_live, 0, static_cast<size_t>(new_cap));

        ptrdiff_t built = 0;
        struct build_guard {
            value_type* base;
            ptrdiff_t* count;
            ~build_guard() {
                for (ptrdiff_t i = 0; i < *count; ++i) {
                    traits::destroy(&base[i]);
                }
            }
        } guard{ fresh_slots, &built };

        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (!live_[i]) {
                continue;
            }
            traits::construct_move_noexcept(&fresh_slots[built], slots_[i]);
            fresh_live[built] = 1;
            ++built;
        }

        built = 0;
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            if (live_[i]) {
                traits::destroy(&slots_[i]);
            }
        }
        if (slots_ != nullptr) {
            df::deallocate(slots_, static_cast<size_t>(cap_) * sizeof(value_type));
        }
        if (live_ != nullptr) {
            df::deallocate(live_, static_cast<size_t>(cap_));
        }

        slots_ = fresh_slots;
        live_ = fresh_live;
        cap_ = new_cap;
        entries_count_ = live_count_;
        live_count_ = entries_count_;

        index_.clear();
        for (ptrdiff_t i = 0; i < entries_count_; ++i) {
            index_.insert(traits::key_of(slots_[i]), static_cast<uint32_t>(i));
        }
    }

    void realloc_slots(ptrdiff_t new_cap) {
        assert(new_cap >= live_count_);
        if (new_cap == cap_ && entries_count_ == live_count_) {
            return;
        }
        if constexpr (trivially_relocatable) {
            realloc_slots_trivial(new_cap);
        } else {
            realloc_slots_strong(new_cap);
        }
    }

    void ensure_room_for_one() {
        if (entries_count_ + 1 <= cap_) {
            return;
        }
        const ptrdiff_t new_cap = cap_ > 0 ? cap_ + cap_ / 2 + 8 : 16;
        realloc_slots(new_cap);
    }

    void maybe_compact() {
        const ptrdiff_t holes = entries_count_ - live_count_;
        if (holes > live_count_) {
            compact();
        }
    }

    template <typename KK>
    place_result place_key(KK&& key) {
        static_assert(is_set_v, "place_key is set-only");
        uint32_t* p = index_.find(key);
        if (p != nullptr) {
            return { static_cast<ptrdiff_t>(*p), false };
        }
        ensure_room_for_one();
        const ptrdiff_t vi = entries_count_;
        traits::construct(&slots_[vi], std::forward<KK>(key));
        live_[vi] = 1;
        ++entries_count_;
        ++live_count_;
        index_.insert(traits::key_of(slots_[vi]), static_cast<uint32_t>(vi));
        return { vi, true };
    }

    template <typename KK, typename VV>
    place_result place_kv(KK&& key, VV&& v) {
        static_assert(!is_set_v, "place_kv is map-only");
        uint32_t* p = index_.find(key);
        if (p != nullptr) {
            return { static_cast<ptrdiff_t>(*p), false };
        }
        ensure_room_for_one();
        const ptrdiff_t vi = entries_count_;
        traits::construct(&slots_[vi], std::forward<KK>(key), std::forward<VV>(v));
        live_[vi] = 1;
        ++entries_count_;
        ++live_count_;
        index_.insert(traits::key_of(slots_[vi]), static_cast<uint32_t>(vi));
        return { vi, true };
    }

    place_result place_default(const K& key) {
        static_assert(!is_set_v, "place_default is map-only");
        ensure_room_for_one();
        const ptrdiff_t vi = entries_count_;
        construct_slot_default(vi, key);
        ++entries_count_;
        ++live_count_;
        index_.insert(traits::key_of(slots_[vi]), static_cast<uint32_t>(vi));
        return { vi, true };
    }

    template <typename KK, typename VV>
    void do_insert_or_assign(KK&& key, VV&& v) {
        static_assert(!is_set_v, "do_insert_or_assign is map-only");
        uint32_t* p = index_.find(key);
        if (p != nullptr) {
            slots_[*p].second = std::forward<VV>(v);
            return;
        }
        place_kv(std::forward<KK>(key), std::forward<VV>(v));
    }
};

template <typename K, typename V>
inline void swap(ord_dense_map<K, V>& a, ord_dense_map<K, V>& b) noexcept {
    a.swap(b);
}

} // namespace df