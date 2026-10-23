#pragma once

// bc_unord_dense_map.h
//
// Open-addressing hash map: dense value array + group-based index.
// Design after ankerl::unordered_dense (MIT, Martin Leitner-Ankerl):
//   - values stored contiguously in a separate array (dense iteration)
//   - index is groups of 16 slots: 16 fingerprints + 8 overflow counters
//     + 16 value indices
//   - quadratic (triangular) probing over groups, no tombstones
//   - erase never rehashes; it decrements the overflow counters it incremented
//   - move_home on writing hits undoes churn drift
//
// IMPORTANT (map only): keys are mutable in the stored pair
// (pair<K, V>, not pair<const K, V>). Do NOT modify a key through an
// iterator or via `it->first` -- doing so corrupts the container
// silently: the node is in the bucket its OLD key hashed to, so
// find()/contains()/erase() for the NEW key all return "not found"
// while the mutated entry still shows up in iteration.
//
// The SET specialization (V == void) does NOT have this problem:
// iterators yield const K&, so `*it = x` is a compile error.
//
// If you need const-key semantics on a map, use df::unordered_map
// (chained) or df::map (red-black).
//
// Hasher: wy::hash_of<K> (wy::hash_cstr for const char* / char*).
// Equality: operator== (strcmp for const char* / char*).
// Pair: df::pair<K, V>.
// Pool-backed via df::allocate / df::deallocate.
//
// Malloc-backed counterpart: sdf::unord_dense_map<K, V> in bcs_unord_dense_map.h.
//
// Style: snake_case, One True Brace Style, template params K, V.

#include "bc_allocator.h"
#include "bc_hash.h"
#include "bc_pair.h"

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
struct unord_dense_value_traits {
    using value_type = pair<K, V>;

    template <typename KK, typename VV>
    static void construct(value_type* at, KK&& k, VV&& v) {
        new (&at->first) K(std::forward<KK>(k));
        new (&at->second) V(std::forward<VV>(v));
    }
    static void construct_default(value_type* at, const K& k) {
        new (&at->first) K(k);
        new (&at->second) V();
    }
    static void destroy(value_type* at) noexcept {
        at->second.~V();
        at->first.~K();
    }
    static K&       key_of(value_type& v)       noexcept { return v.first; }
    static const K& key_of(const value_type& v) noexcept { return v.first; }
};

template <typename K>
struct unord_dense_value_traits<K, void> {
    using value_type = K;

    template <typename KK, typename = std::enable_if_t<!std::is_same_v<std::decay_t<KK>, K>>>
    static void construct(value_type* at, KK&& k) {
        new (at) K(std::forward<KK>(k));
    }
    static void construct(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void construct(value_type* at, K&& k) {
        new (at) K(std::move(k));
    }
    static void construct_default(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void destroy(value_type* at) noexcept {
        at->~K();
    }
    static K&       key_of(value_type& v)       noexcept { return v; }
    static const K& key_of(const value_type& v) noexcept { return v; }
};

} // namespace detail

template <typename K, typename V>
class unord_dense_map {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = typename detail::unord_dense_value_traits<K, V>::value_type;
    using size_type   = size_t;

    static constexpr ptrdiff_t min_groups       = 4;
    static constexpr ptrdiff_t slots_per_group  = 16;
    static constexpr ptrdiff_t counters_per_grp = 8;
    static constexpr float     default_load     = 0.8f;

    static constexpr bool is_set_v = std::is_void_v<V>;

private:
    struct group {
        uint8_t  fingerprints[slots_per_group];
        uint8_t  overflows[counters_per_grp];
        uint32_t index[slots_per_group];
    };
    static_assert(sizeof(group) == 88, "group must stay 88 bytes");

    using traits = detail::unord_dense_value_traits<K, V>;

    static constexpr bool trivially_relocatable =
        std::is_trivially_copyable_v<value_type>    &&
        std::is_trivially_destructible_v<value_type>;

    struct place_result {
        ptrdiff_t value_idx;
        bool      inserted;
    };

    value_type* values_      = nullptr;
    group*      groups_      = nullptr;
    ptrdiff_t   len_         = 0;
    ptrdiff_t   cap_         = 0;
    ptrdiff_t   group_count_ = 0;
    ptrdiff_t   group_mask_  = 0;
    ptrdiff_t   max_values_  = 0;
    ptrdiff_t   shifts_      = 64 - 2;
    float       max_load_    = default_load;

public:
    // ---- lifecycle -------------------------------------------------------

    unord_dense_map() noexcept {}

    unord_dense_map(const unord_dense_map&) = delete;
    unord_dense_map& operator=(const unord_dense_map&) = delete;

    unord_dense_map(unord_dense_map&& other) noexcept {
        steal(other);
    }

    unord_dense_map& operator=(unord_dense_map&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~unord_dense_map() {
        destroy_all();
    }

    // ---- capacity --------------------------------------------------------

    size_t    size()      const noexcept { return static_cast<size_t>(len_); }
    ptrdiff_t ssize()     const noexcept { return len_; }
    ptrdiff_t scapacity() const noexcept { return cap_; }
    size_t    capacity()  const noexcept { return static_cast<size_t>(cap_); }
    bool      empty()     const noexcept { return len_ == 0; }

    void reserve(ptrdiff_t n) {
        if (n <= 0) {
            return;
        }
        ptrdiff_t want_groups = ptrdiff_t(1);
        const ptrdiff_t want_values = n;
        while (static_cast<float>(want_groups * slots_per_group) * max_load_ < want_values) {
            want_groups <<= 1;
        }
        if (want_groups <= group_count_) {
            return;
        }
        rehash_to(want_groups);
    }

    void clear() noexcept {
        if (values_ == nullptr) {
            return;
        }
        destroy_range(0, len_);
        len_ = 0;
        if (groups_ != nullptr) {
            memset(groups_, 0, static_cast<size_t>(group_count_) * sizeof(group));
        }
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

    bool try_insert(const K& key) {
        static_assert(is_set_v, "try_insert(key) requires V == void; use try_insert(key, value)");
        place_result r = place_key_only(key);
        return r.inserted;
    }
    bool try_insert(K&& key) {
        static_assert(is_set_v, "try_insert(key) requires V == void; use try_insert(key, value)");
        place_result r = place_key_only(std::move(key));
        return r.inserted;
    }
    bool try_insert(const K& key, const V& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        place_result r = place_value(key, v);
        return r.inserted;
    }
    bool try_insert(K&& key, V&& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        place_result r = place_value(std::move(key), std::move(v));
        return r.inserted;
    }

    V& operator[](const K& key) {
        static_assert(!is_set_v, "operator[] is not available on a set");
        const uint64_t mh = mixed_hash(key);
        ptrdiff_t value_idx = find_value_index_hashed(key, mh);
        if (value_idx >= 0) {
            move_home(mh, static_cast<uint32_t>(value_idx));
            return values_[value_idx].second;
        }
        place_result r = place_default(key, mh);
        return values_[r.value_idx].second;
    }

    bool erase(const K& key) {
        ptrdiff_t value_idx = find_value_index(key);
        if (value_idx < 0) {
            return false;
        }
        erase_value_at(value_idx);
        return true;
    }

    void rehash(ptrdiff_t new_group_count) {
        ptrdiff_t n = 1;
        while (n < new_group_count) {
            n <<= 1;
        }
        if (n < min_groups) {
            n = min_groups;
        }
        if (n == group_count_ && groups_ != nullptr) {
            memset(groups_, 0, static_cast<size_t>(group_count_) * sizeof(group));
            rebuild_index();
            return;
        }
        rehash_to(n);
    }

    // ---- lookup ----------------------------------------------------------

    value_type* find(const K& key) noexcept {
        ptrdiff_t i = find_value_index(key);
        return i >= 0 ? &values_[i] : nullptr;
    }
    const value_type* find(const K& key) const noexcept {
        ptrdiff_t i = find_value_index(key);
        return i >= 0 ? &values_[i] : nullptr;
    }

    bool contains(const K& key) const noexcept {
        return find_value_index(key) >= 0;
    }

    size_t count(const K& key) const noexcept {
        return find_value_index(key) >= 0 ? 1 : 0;
    }

    V& at(const K& key) {
        static_assert(!is_set_v, "at() is not available on a set");
        ptrdiff_t i = find_value_index(key);
        assert(i >= 0 && "unord_dense_map::at key not found");
        return values_[i].second;
    }
    const V& at(const K& key) const {
        static_assert(!is_set_v, "at() is not available on a set");
        ptrdiff_t i = find_value_index(key);
        assert(i >= 0 && "unord_dense_map::at key not found");
        return values_[i].second;
    }

    V& get_or_insert(const K& key, const V& default_value) {
        static_assert(!is_set_v, "get_or_insert is not available on a set");
        ptrdiff_t i = find_value_index(key);
        if (i >= 0) {
            return values_[i].second;
        }
        place_result r = place_value(key, default_value);
        return values_[r.value_idx].second;
    }

    // ---- iteration -------------------------------------------------------

    template <bool IsConst>
    class iter_t {
        using slot_ptr = std::conditional_t<IsConst, const value_type*, value_type*>;
        using value_ref = std::conditional_t<
            IsConst || is_set_v,
            const value_type&,
            value_type&>;
        using value_pp = std::conditional_t<
            IsConst || is_set_v,
            const value_type*,
            value_type*>;

        slot_ptr p_ = nullptr;

    public:
        using value_type        = unord_dense_map::value_type;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::forward_iterator_tag;

        iter_t() noexcept = default;
        explicit iter_t(slot_ptr p) noexcept : p_(p) {}

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept : p_(other.p_) {}

        reference operator*()  const noexcept { return *p_; }
        pointer   operator->() const noexcept { return p_; }

        iter_t& operator++() noexcept { ++p_; return *this; }
        iter_t operator++(int) noexcept { iter_t t(*this); ++(*this); return t; }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return p_ == o.p_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return p_ != o.p_; }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    iterator       begin()       noexcept { return iterator(values_); }
    iterator       end()         noexcept { return iterator(values_ + len_); }
    const_iterator begin() const noexcept { return const_iterator(values_); }
    const_iterator end()   const noexcept { return const_iterator(values_ + len_); }
    const_iterator cbegin() const noexcept { return const_iterator(values_); }
    const_iterator cend()   const noexcept { return const_iterator(values_ + len_); }

    template <typename Fn>
    void for_each(Fn&& fn) {
        if constexpr (is_set_v) {
            for (ptrdiff_t i = 0; i < len_; ++i) {
                fn(static_cast<const K&>(traits::key_of(values_[i])));
            }
        } else {
            for (ptrdiff_t i = 0; i < len_; ++i) {
                fn(const_cast<const K&>(values_[i].first), values_[i].second);
            }
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        if constexpr (is_set_v) {
            for (ptrdiff_t i = 0; i < len_; ++i) {
                fn(traits::key_of(values_[i]));
            }
        } else {
            for (ptrdiff_t i = 0; i < len_; ++i) {
                fn(traits::key_of(values_[i]), values_[i].second);
            }
        }
    }

    void swap(unord_dense_map& other) noexcept {
        std::swap(values_, other.values_);
        std::swap(groups_, other.groups_);
        std::swap(len_, other.len_);
        std::swap(cap_, other.cap_);
        std::swap(group_count_, other.group_count_);
        std::swap(group_mask_, other.group_mask_);
        std::swap(max_values_, other.max_values_);
        std::swap(shifts_, other.shifts_);
        std::swap(max_load_, other.max_load_);
    }

private:
    // ---- hashing / equality ----------------------------------------------

    static uint64_t mixed_hash(const K& key) {
        if constexpr (std::is_same_v<K, const char*> || std::is_same_v<K, char*>) {
            return wy::hash_cstr(key, 0);
        } else {
            return wy::hash_of(key, 0);
        }
    }

    static bool key_equal(const K& a, const K& b) {
        if constexpr (std::is_same_v<K, const char*> || std::is_same_v<K, char*>) {
            return strcmp(a, b) == 0;
        } else {
            return a == b;
        }
    }

    static uint8_t fingerprint_of(uint64_t h) noexcept {
        uint8_t f = uint8_t(h & 0xFF);
        return f == 0 ? 8 : f;
    }

    ptrdiff_t home_group(uint64_t h) const noexcept {
        return static_cast<ptrdiff_t>(h >> shifts_);
    }

    ptrdiff_t next_group(ptrdiff_t g, ptrdiff_t& delta) const noexcept {
        ++delta;
        return (g + delta) & group_mask_;
    }

    static const K& key_of(const value_type& v) noexcept { return traits::key_of(v); }

    // ---- allocation ------------------------------------------------------

    void free_values() noexcept {
        if (values_ != nullptr) {
            df::deallocate(values_, static_cast<size_t>(cap_) * sizeof(value_type));
            values_ = nullptr;
        }
        cap_ = 0;
    }

    void alloc_groups(ptrdiff_t n) {
        void* raw = df::allocate(static_cast<size_t>(n) * sizeof(group));
        assert(raw != nullptr && "unord_dense_map: groups allocation failed");
        groups_ = static_cast<group*>(raw);
        memset(groups_, 0, static_cast<size_t>(n) * sizeof(group));
        group_count_ = n;
        group_mask_ = n - 1;
    }

    void free_groups() noexcept {
        if (groups_ != nullptr) {
            df::deallocate(groups_, static_cast<size_t>(group_count_) * sizeof(group));
            groups_ = nullptr;
        }
        group_count_ = 0;
        group_mask_ = 0;
    }

    // ---- destruction -----------------------------------------------------

    static void destroy_value(value_type& v) noexcept {
        traits::destroy(&v);
    }

    void destroy_range(ptrdiff_t first, ptrdiff_t last) noexcept {
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            for (ptrdiff_t i = first; i < last; ++i) {
                destroy_value(values_[i]);
            }
        }
    }

    void destroy_all() noexcept {
        if (values_ != nullptr) {
            destroy_range(0, len_);
        }
        free_values();
        free_groups();
        len_ = 0;
    }

    void steal(unord_dense_map& other) noexcept {
        values_      = other.values_;
        groups_      = other.groups_;
        len_         = other.len_;
        cap_         = other.cap_;
        group_count_ = other.group_count_;
        group_mask_  = other.group_mask_;
        max_values_  = other.max_values_;
        shifts_      = other.shifts_;
        max_load_    = other.max_load_;

        other.values_      = nullptr;
        other.groups_      = nullptr;
        other.len_         = 0;
        other.cap_         = 0;
        other.group_count_ = 0;
        other.group_mask_  = 0;
        other.max_values_  = 0;
        other.shifts_      = 64 - 2;
    }

    // ---- growth ----------------------------------------------------------

    ptrdiff_t calc_shifts_for_groups(ptrdiff_t n_groups) const noexcept {
        ptrdiff_t s = 64;
        ptrdiff_t current = ptrdiff_t(1);
        while (current < n_groups && s > 0) {
            current <<= 1;
            --s;
        }
        return s;
    }

    ptrdiff_t grow_values_cap(ptrdiff_t need) const noexcept {
        ptrdiff_t c = cap_ > 0 ? cap_ : 8;
        while (c < need) {
            c = c + c / 2;
        }
        return c;
    }

    void grow_values_trivial(ptrdiff_t new_cap) {
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(value_type));
        assert(raw != nullptr && "unord_dense_map: values allocation failed");
        value_type* fresh = static_cast<value_type*>(raw);
        if (len_ > 0) {
            memcpy(fresh, values_, static_cast<size_t>(len_) * sizeof(value_type));
        }
        if (values_ != nullptr) {
            df::deallocate(values_, static_cast<size_t>(cap_) * sizeof(value_type));
        }
        values_ = fresh;
        cap_ = new_cap;
    }

    void grow_values_strong(ptrdiff_t new_cap) {
        void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(value_type));
        assert(raw != nullptr && "unord_dense_map: values allocation failed");
        value_type* fresh = static_cast<value_type*>(raw);

        ptrdiff_t built = 0;
        struct build_guard {
            value_type* base;
            ptrdiff_t* count;
            ~build_guard() {
                for (ptrdiff_t i = 0; i < *count; ++i) {
                    traits::destroy(&base[i]);
                }
            }
        } guard{ fresh, &built };

        for (ptrdiff_t i = 0; i < len_; ++i) {
            if constexpr (is_set_v) {
                new (&fresh[i]) K(std::move_if_noexcept(values_[i]));
            } else {
                new (&fresh[i].first)  K(std::move_if_noexcept(values_[i].first));
                new (&fresh[i].second) V(std::move_if_noexcept(values_[i].second));
            }
            ++built;
        }

        built = 0;
        for (ptrdiff_t i = 0; i < len_; ++i) {
            destroy_value(values_[i]);
        }
        if (values_ != nullptr) {
            df::deallocate(values_, static_cast<size_t>(cap_) * sizeof(value_type));
        }
        values_ = fresh;
        cap_ = new_cap;
    }

    void grow_values(ptrdiff_t new_cap) {
        if constexpr (trivially_relocatable) {
            grow_values_trivial(new_cap);
        } else {
            grow_values_strong(new_cap);
        }
    }

    void rehash_to(ptrdiff_t new_group_count) {
        void* raw = df::allocate(static_cast<size_t>(new_group_count) * sizeof(group));
        assert(raw != nullptr && "unord_dense_map: groups allocation failed");
        group* fresh = static_cast<group*>(raw);
        memset(fresh, 0, static_cast<size_t>(new_group_count) * sizeof(group));

        group*    old_groups = groups_;
        ptrdiff_t old_count  = group_count_;

        groups_      = fresh;
        group_count_ = new_group_count;
        group_mask_  = new_group_count - 1;
        shifts_      = calc_shifts_for_groups(new_group_count);

        for (ptrdiff_t i = 0; i < len_; ++i) {
            place_index(mixed_hash(key_of(values_[i])), static_cast<uint32_t>(i));
        }

        if (old_groups != nullptr) {
            df::deallocate(old_groups, static_cast<size_t>(old_count) * sizeof(group));
        }
    }

    void ensure_capacity_for_one() {
        if (groups_ == nullptr) {
            alloc_groups(min_groups);
            shifts_ = calc_shifts_for_groups(min_groups);
            max_values_ = static_cast<ptrdiff_t>(static_cast<float>(group_count_ * slots_per_group) * max_load_);
            if (max_values_ < 1) {
                max_values_ = 1;
            }
        }
        if (len_ + 1 > max_values_) {
            rehash_to(group_count_ * 2);
            max_values_ = static_cast<ptrdiff_t>(static_cast<float>(group_count_ * slots_per_group) * max_load_);
        }
        if (len_ + 1 > cap_) {
            grow_values(grow_values_cap(len_ + 1));
        }
    }

    // ---- index manipulation ---------------------------------------------

    void place_index(uint64_t mh, uint32_t value_idx) {
        const uint8_t fp = fingerprint_of(mh);
        const unsigned counter = fp & 7u;
        ptrdiff_t g = home_group(mh);
        ptrdiff_t delta = 0;
        while (true) {
            group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] == 0) {
                    grp.fingerprints[lane] = fp;
                    grp.index[lane] = value_idx;
                    return;
                }
            }
            if (grp.overflows[counter] != 255) {
                ++grp.overflows[counter];
            }
            g = next_group(g, delta);
        }
    }

    void clear_index_slot(uint64_t mh, uint32_t value_idx) {
        const uint8_t fp = fingerprint_of(mh);
        const unsigned counter = fp & 7u;
        ptrdiff_t home = home_group(mh);
        ptrdiff_t g = home;
        ptrdiff_t delta = 0;
        while (true) {
            group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] == fp && grp.index[lane] == value_idx) {
                    grp.fingerprints[lane] = 0;
                    ptrdiff_t w = home;
                    ptrdiff_t d = 0;
                    while (w != g) {
                        if (groups_[w].overflows[counter] != 255) {
                            --groups_[w].overflows[counter];
                        }
                        w = next_group(w, d);
                    }
                    return;
                }
            }
            g = next_group(g, delta);
            if (g == home) {
                return;
            }
        }
    }

    void repoint_index(uint64_t mh, uint32_t old_idx, uint32_t new_idx) {
        const uint8_t fp = fingerprint_of(mh);
        ptrdiff_t g = home_group(mh);
        ptrdiff_t delta = 0;
        while (true) {
            group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] == fp && grp.index[lane] == old_idx) {
                    grp.index[lane] = new_idx;
                    return;
                }
            }
            g = next_group(g, delta);
            if (g == home_group(mh)) {
                return;
            }
        }
    }

    void rebuild_index() {
        for (ptrdiff_t i = 0; i < len_; ++i) {
            place_index(mixed_hash(key_of(values_[i])), static_cast<uint32_t>(i));
        }
    }

    // ---- lookup engine ---------------------------------------------------

    ptrdiff_t find_value_index_hashed(const K& key, uint64_t mh) const noexcept {
        if (groups_ == nullptr) {
            return -1;
        }
        const uint8_t fp = fingerprint_of(mh);
        const unsigned counter = fp & 7u;
        ptrdiff_t g = home_group(mh);
        ptrdiff_t delta = 0;
        while (true) {
            const group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] != fp) {
                    continue;
                }
                const uint32_t value_idx = grp.index[lane];
                if (key_equal(key, key_of(values_[value_idx]))) {
                    return static_cast<ptrdiff_t>(value_idx);
                }
            }
            if (grp.overflows[counter] == 0 || delta == group_mask_) {
                return -1;
            }
            g = next_group(g, delta);
        }
    }

    ptrdiff_t find_value_index(const K& key) const noexcept {
        return find_value_index_hashed(key, mixed_hash(key));
    }

    // ---- move_home -------------------------------------------------------

    void move_home(uint64_t mh, uint32_t value_idx) {
        const ptrdiff_t home = home_group(mh);
        const uint8_t fp = fingerprint_of(mh);

        ptrdiff_t found_group = home;
        ptrdiff_t found_lane  = -1;
        {
            ptrdiff_t g = home;
            ptrdiff_t delta = 0;
            while (true) {
                const group& grp = groups_[g];
                for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                    if (grp.fingerprints[lane] == fp && grp.index[lane] == value_idx) {
                        found_group = g;
                        found_lane = lane;
                        break;
                    }
                }
                if (found_lane >= 0) {
                    break;
                }
                if (delta == group_mask_) {
                    assert(false && "unord_dense_map: move_home could not find slot");
                    return;
                }
                g = next_group(g, delta);
            }
        }
        if (found_group == home) {
            return;
        }

        group& home_grp = groups_[home];
        ptrdiff_t empty_lane = -1;
        for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
            if (home_grp.fingerprints[lane] == 0) {
                empty_lane = lane;
                break;
            }
        }
        if (empty_lane < 0) {
            return;
        }

        group& from_grp = groups_[found_group];
        home_grp.fingerprints[empty_lane] = from_grp.fingerprints[found_lane];
        home_grp.index[empty_lane]        = from_grp.index[found_lane];
        from_grp.fingerprints[found_lane] = 0;

        const unsigned counter = fp & 7u;
        ptrdiff_t w = home;
        ptrdiff_t d = 0;
        while (w != found_group) {
            if (groups_[w].overflows[counter] != 255) {
                --groups_[w].overflows[counter];
            }
            w = next_group(w, d);
        }
    }

    // ---- insert helpers --------------------------------------------------

    void fix_counters_on_insert(ptrdiff_t home, ptrdiff_t found, unsigned counter) {
        ptrdiff_t w = home;
        ptrdiff_t d = 0;
        while (w != found) {
            if (groups_[w].overflows[counter] != 255) {
                ++groups_[w].overflows[counter];
            }
            w = next_group(w, d);
        }
    }

    template <typename KK, typename VV>
    place_result place_value(KK&& key, VV&& v) {
        static_assert(!is_set_v, "place_value is map-only");
        ensure_capacity_for_one();
        const uint64_t mh = mixed_hash(key);
        const uint8_t fp = fingerprint_of(mh);
        const unsigned counter = fp & 7u;
        ptrdiff_t g = home_group(mh);
        ptrdiff_t delta = 0;
        while (true) {
            group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] == fp) {
                    const uint32_t value_idx = grp.index[lane];
                    if (key_equal(key, key_of(values_[value_idx]))) {
                        return { static_cast<ptrdiff_t>(value_idx), false };
                    }
                } else if (grp.fingerprints[lane] == 0) {
                    const ptrdiff_t vi = len_;
                    traits::construct(&values_[vi], std::forward<KK>(key), std::forward<VV>(v));
                    ++len_;
                    grp.fingerprints[lane] = fp;
                    grp.index[lane] = static_cast<uint32_t>(vi);
                    fix_counters_on_insert(home_group(mh), g, counter);
                    return { vi, true };
                }
            }
            if (grp.overflows[counter] != 255) {
                ++grp.overflows[counter];
            }
            g = next_group(g, delta);
        }
    }

    template <typename KK>
    place_result place_key_only(KK&& key) {
        static_assert(is_set_v, "place_key_only is set-only");
        ensure_capacity_for_one();
        const uint64_t mh = mixed_hash(key);
        const uint8_t fp = fingerprint_of(mh);
        const unsigned counter = fp & 7u;
        ptrdiff_t g = home_group(mh);
        ptrdiff_t delta = 0;
        while (true) {
            group& grp = groups_[g];
            for (ptrdiff_t lane = 0; lane < slots_per_group; ++lane) {
                if (grp.fingerprints[lane] == fp) {
                    const uint32_t value_idx = grp.index[lane];
                    if (key_equal(key, key_of(values_[value_idx]))) {
                        return { static_cast<ptrdiff_t>(value_idx), false };
                    }
                } else if (grp.fingerprints[lane] == 0) {
                    const ptrdiff_t vi = len_;
                    new (&values_[vi]) K(std::forward<KK>(key));
                    ++len_;
                    grp.fingerprints[lane] = fp;
                    grp.index[lane] = static_cast<uint32_t>(vi);
                    fix_counters_on_insert(home_group(mh), g, counter);
                    return { vi, true };
                }
            }
            if (grp.overflows[counter] != 255) {
                ++grp.overflows[counter];
            }
            g = next_group(g, delta);
        }
    }

    place_result place_default(const K& key, uint64_t mh) {
        static_assert(!is_set_v, "place_default is map-only");
        ensure_capacity_for_one();
        const ptrdiff_t vi = len_;
        traits::construct_default(&values_[vi], key);
        ++len_;
        place_index(mh, static_cast<uint32_t>(vi));
        return { vi, true };
    }

    template <typename KK, typename VV>
    void do_insert_or_assign(KK&& key, VV&& v) {
        static_assert(!is_set_v, "do_insert_or_assign is map-only");
        const uint64_t mh = mixed_hash(key);
        place_result r = place_value(std::forward<KK>(key), std::forward<VV>(v));
        if (r.inserted) {
            return;
        }
        values_[r.value_idx].second = std::forward<VV>(v);
        move_home(mh, static_cast<uint32_t>(r.value_idx));
    }

    // ---- erase -----------------------------------------------------------

    void erase_value_at(ptrdiff_t value_idx) {
        const uint64_t mh = mixed_hash(key_of(values_[value_idx]));
        clear_index_slot(mh, static_cast<uint32_t>(value_idx));

        const ptrdiff_t last = len_ - 1;
        if (value_idx != last) {
            if constexpr (is_set_v) {
                values_[value_idx].~K();
                new (&values_[value_idx]) K(std::move(values_[last]));
                const uint64_t moved_mh = mixed_hash(values_[value_idx]);
                repoint_index(moved_mh, static_cast<uint32_t>(last), static_cast<uint32_t>(value_idx));
                values_[last].~K();
            } else {
                values_[value_idx].first  = std::move(values_[last].first);
                values_[value_idx].second = std::move(values_[last].second);
                const uint64_t moved_mh = mixed_hash(values_[value_idx].first);
                repoint_index(moved_mh, static_cast<uint32_t>(last), static_cast<uint32_t>(value_idx));
                destroy_value(values_[last]);
            }
        } else {
            destroy_value(values_[last]);
        }
        --len_;
    }
};

template <typename K, typename V>
inline void swap(unord_dense_map<K, V>& a, unord_dense_map<K, V>& b) noexcept {
    a.swap(b);
}

} // namespace df