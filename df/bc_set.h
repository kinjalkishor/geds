#pragma once

// bc_set.h
//
// Ordered tree set, a thin wrapper over df::map<K, void>.
// Mirrors the shape of df::unordered_set / df::unord_dense_set /
// df::ord_dense_set so callers can swap between the four set implementations
// with no API change -- except that set keeps its elements sorted (in-order
// walk), while the hash-backed sets have implementation-defined or
// insertion order.
//
// When to use this over the hash sets:
//   - you need sorted iteration (for_each walks in ascending key order)
//   - you need ordered queries: lower_bound, upper_bound, equal_range,
//     min, max
//   - interleaved insert + erase + ordered query, where the set cannot be
//     built once and read many (for that, a sorted vector + binary_search
//     beats a red-black tree)
//   - small sets where O(log n) with a cache-hostile walk is fine
//
// Keys are const: the iterator yields const K&, find() returns const K*.
//
// count() returns size_t, matching std::set::count.
//
// Pool-backed via df::allocate / df::deallocate.
//
// Style: snake_case, One True Brace Style, template params K, Fn.

#include "bc_map.h"

#include <stddef.h>

namespace df {

template <typename K>
class set {
public:
    using key_type    = K;
    using value_type  = K;
    using size_type   = size_t;

private:
    using impl_type = map<K, void>;
    impl_type impl_;

public:
    set() noexcept = default;

    set(const set&) = delete;
    set& operator=(const set&) = delete;

    set(set&& other) noexcept
        : impl_(std::move(other.impl_)) {}

    set& operator=(set&& other) noexcept {
        impl_ = std::move(other.impl_);
        return *this;
    }

    ~set() = default;

    size_t    size()  const noexcept { return impl_.size(); }
    ptrdiff_t ssize() const noexcept { return impl_.ssize(); }
    bool      empty() const noexcept { return impl_.empty(); }

    void clear() noexcept { impl_.clear(); }

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

    typename impl_type::iterator erase(typename impl_type::const_iterator pos) {
        return impl_.erase(pos);
    }

    typename impl_type::iterator lower_bound(const K& key) noexcept {
        return impl_.lower_bound(key);
    }
    typename impl_type::const_iterator lower_bound(const K& key) const noexcept {
        return impl_.lower_bound(key);
    }
    typename impl_type::iterator upper_bound(const K& key) noexcept {
        return impl_.upper_bound(key);
    }
    typename impl_type::const_iterator upper_bound(const K& key) const noexcept {
        return impl_.upper_bound(key);
    }
    pair<typename impl_type::iterator, typename impl_type::iterator>
    equal_range(const K& key) noexcept {
        return impl_.equal_range(key);
    }
    pair<typename impl_type::const_iterator, typename impl_type::const_iterator>
    equal_range(const K& key) const noexcept {
        return impl_.equal_range(key);
    }
    typename impl_type::iterator min() noexcept { return impl_.min(); }
    typename impl_type::const_iterator min() const noexcept { return impl_.min(); }
    typename impl_type::iterator max() noexcept { return impl_.max(); }
    typename impl_type::const_iterator max() const noexcept { return impl_.max(); }

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

    bool is_subset_of(const set& other) const {
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

    void union_with(const set& other) {
        if (&other == this) {
            return;
        }
        for (const K& k : other) {
            insert(k);
        }
    }

    void intersect(const set& other) {
        if (empty() || other.empty()) {
            clear();
            return;
        }
        auto it = begin();
        auto last = end();
        while (it != last) {
            if (!other.contains(*it)) {
                it = erase(it);
            } else {
                ++it;
            }
        }
    }

    void difference_with(const set& other) {
        if (empty() || other.empty()) {
            return;
        }
        auto it = begin();
        auto last = end();
        while (it != last) {
            if (other.contains(*it)) {
                it = erase(it);
            } else {
                ++it;
            }
        }
    }

    void swap(set& other) noexcept {
        impl_.swap(other.impl_);
    }
};

template <typename K>
inline void swap(set<K>& a, set<K>& b) noexcept {
    a.swap(b);
}

} // namespace df