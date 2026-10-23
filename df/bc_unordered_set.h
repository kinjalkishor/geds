#pragma once

// bc_unordered_set.h
//
// Chained hash set, a thin wrapper over df::unordered_map<K, void>.
// Mirrors std::unordered_set semantics: nodes never move on insert,
// const keys through iterators, only the erased iterator is invalidated.
//
// Use this when you specifically need iterator / reference stability on
// insert. For general set use prefer df::unord_dense_set (open addressing),
// which is smaller, faster, and denser.
//
// count() returns size_t, matching std::unordered_set::count.
//
// Pool-backed via df::allocate / df::deallocate.
//
// Style: snake_case, One True Brace Style, template params K, It, Fn.

#include "bc_unordered_map.h"
#include "bc_vector.h"

#include <stddef.h>

namespace df {

template <typename K>
class unordered_set {
public:
    using key_type    = K;
    using value_type  = K;
    using size_type   = size_t;

private:
    using impl_type = unordered_map<K, void>;
    impl_type impl_;

public:
    unordered_set() noexcept = default;

    unordered_set(const unordered_set&) = delete;
    unordered_set& operator=(const unordered_set&) = delete;

    unordered_set(unordered_set&& other) noexcept
        : impl_(std::move(other.impl_)) {}

    unordered_set& operator=(unordered_set&& other) noexcept {
        impl_ = std::move(other.impl_);
        return *this;
    }

    ~unordered_set() = default;

    size_t    size()           const noexcept { return impl_.size(); }
    ptrdiff_t ssize()          const noexcept { return impl_.ssize(); }
    size_t    bucket_count()   const noexcept { return impl_.bucket_count(); }
    ptrdiff_t sbucket_count()  const noexcept { return impl_.sbucket_count(); }
    bool      empty()          const noexcept { return impl_.empty(); }

    void reserve(ptrdiff_t n) { impl_.reserve(n); }
    void rehash(ptrdiff_t n)  { impl_.rehash(n); }
    void clear() noexcept     { impl_.clear(); }

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

    bool contains(const K& key) const noexcept { return impl_.contains(key); }
    size_t count(const K& key)  const noexcept { return impl_.contains(key) ? 1 : 0; }

    const K* find(const K& key) noexcept       { return impl_.find(key); }
    const K* find(const K& key) const noexcept { return impl_.find(key); }

    bool insert(const K& key) { return impl_.insert(key); }
    bool insert(K&& key)      { return impl_.insert(std::move(key)); }

    bool erase(const K& key) { return impl_.erase(key); }

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

    bool is_subset_of(const unordered_set& other) const {
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

    void union_with(const unordered_set& other) {
        if (&other == this) {
            return;
        }
        reserve(ssize() + other.ssize());
        for (const K& k : other) {
            insert(k);
        }
    }

    void intersect(const unordered_set& other) {
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

    void difference_with(const unordered_set& other) {
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

    void swap(unordered_set& other) noexcept {
        impl_.swap(other.impl_);
    }
};

template <typename K>
inline void swap(unordered_set<K>& a, unordered_set<K>& b) noexcept {
    a.swap(b);
}

} // namespace df