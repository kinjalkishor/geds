#pragma once

// bc_unord_dense_set.h
//
// Open-addressing hash set, a thin wrapper over df::unord_dense_map<K, void>.
// The V == void specialization stores K directly in the values array, and
// its iterators yield const K&, so `*it = x` is a compile error. This
// wrapper forwards the set-shaped API and adds the set-specific
// conveniences (insert_all, is_subset_of, union_with, intersect,
// difference_with).
//
// Keys are const: iterator yields const K&, find() returns const K*.
// count() returns size_t, matching std::unordered_set::count.
//
// Pool-backed via df::allocate / df::deallocate.
//
// Malloc-backed counterpart: sdf::unord_dense_set<K> in bcs_unord_dense_set.h.
//
// Style: snake_case, One True Brace Style, template params K, It, Fn.

#include "bc_unord_dense_map.h"
#include "bc_vector.h"

#include <stddef.h>

namespace df {

template <typename K>
class unord_dense_set {
public:
    using key_type    = K;
    using value_type  = K;
    using size_type   = size_t;

private:
    using impl_type = unord_dense_map<K, void>;
    impl_type impl_;

public:
    // ---- lifecycle -------------------------------------------------------

    unord_dense_set() noexcept = default;

    unord_dense_set(const unord_dense_set&) = delete;
    unord_dense_set& operator=(const unord_dense_set&) = delete;

    unord_dense_set(unord_dense_set&& other) noexcept
        : impl_(std::move(other.impl_)) {}

    unord_dense_set& operator=(unord_dense_set&& other) noexcept {
        impl_ = std::move(other.impl_);
        return *this;
    }

    ~unord_dense_set() = default;

    // ---- capacity --------------------------------------------------------

    size_t    size()      const noexcept { return impl_.size(); }
    ptrdiff_t ssize()     const noexcept { return impl_.ssize(); }
    ptrdiff_t scapacity() const noexcept { return impl_.scapacity(); }
    size_t    capacity()  const noexcept { return impl_.capacity(); }
    bool      empty()     const noexcept { return impl_.empty(); }

    void reserve(ptrdiff_t n) { impl_.reserve(n); }
    void rehash(ptrdiff_t n)  { impl_.rehash(n); }
    void clear() noexcept     { impl_.clear(); }

    // ---- iterators -------------------------------------------------------

    typename impl_type::iterator       begin()       noexcept { return impl_.begin(); }
    typename impl_type::iterator       end()         noexcept { return impl_.end(); }
    typename impl_type::const_iterator begin() const noexcept { return impl_.begin(); }
    typename impl_type::const_iterator end()   const noexcept { return impl_.end(); }
    typename impl_type::const_iterator cbegin() const noexcept { return impl_.cbegin(); }
    typename impl_type::const_iterator cend()   const noexcept { return impl_.cend(); }

    template <typename Fn>
    void for_each(Fn&& fn) {
        impl_.for_each([&](const K& k) { fn(k); });
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        impl_.for_each([&](const K& k) { fn(k); });
    }

    // ---- lookup ----------------------------------------------------------

    bool contains(const K& key) const noexcept { return impl_.contains(key); }
    size_t count(const K& key)  const noexcept { return impl_.contains(key) ? 1 : 0; }

    const K* find(const K& key) noexcept       { return impl_.find(key); }
    const K* find(const K& key) const noexcept { return impl_.find(key); }

    // ---- modifiers -------------------------------------------------------

    bool insert(const K& key) { return impl_.try_insert(key); }
    bool insert(K&& key)      { return impl_.try_insert(std::move(key)); }

    bool erase(const K& key) { return impl_.erase(key); }

    // ---- set-specific conveniences ---------------------------------------

    template <typename It>
    ptrdiff_t insert_all(It first, It last) {
        ptrdiff_t inserted = 0;
        for (; first != last; ++first) {
            if (insert(*first)) {
                ++inserted;
            }
        }
        return inserted;
    }

    bool is_subset_of(const unord_dense_set& other) const {
        if (size() > other.size()) {
            return false;
        }
        for (const K& k : *this) {
            if (!other.contains(k)) {
                return false;
            }
        }
        return true;
    }

    void union_with(const unord_dense_set& other) {
        if (&other == this) {
            return;
        }
        reserve(ssize() + other.ssize());
        for (const K& k : other) {
            insert(k);
        }
    }

    void intersect(const unord_dense_set& other) {
        if (empty() || other.empty()) {
            clear();
            return;
        }
        df::vector<K> to_remove;
        to_remove.reserve(ssize());
        for (const K& k : *this) {
            if (!other.contains(k)) {
                to_remove.push_back(k);
            }
        }
        for (const K& k : to_remove) {
            erase(k);
        }
    }

    void difference_with(const unord_dense_set& other) {
        if (empty() || other.empty()) {
            return;
        }
        df::vector<K> to_remove;
        for (const K& k : *this) {
            if (other.contains(k)) {
                to_remove.push_back(k);
            }
        }
        for (const K& k : to_remove) {
            erase(k);
        }
    }

    // ---- swap ------------------------------------------------------------

    void swap(unord_dense_set& other) noexcept {
        impl_.swap(other.impl_);
    }
};

template <typename K>
inline void swap(unord_dense_set<K>& a, unord_dense_set<K>& b) noexcept {
    a.swap(b);
}

} // namespace df