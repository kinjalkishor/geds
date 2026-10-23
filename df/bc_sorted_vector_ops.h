#pragma once

// bc_sorted_vector_ops.h
//
// Free functions for the "sorted vector + binary search" pattern: sort
// once after bulk construction, lookup with std::lower_bound, insert/erase
// keeping sorted order. For small-to-moderate N (say <= 500), this beats
// df::map because the vector is dense (one cache line holds several
// elements) and binary search is fast. Above that, df::map's O(log n)
// insert/erase usually wins.
//
// The vector is a plain df::vector<T>. This header does NOT introduce a
// container -- it provides functions on vector<T> for callers that have
// adopted the sorted-vector convention. The convention is:
//   - sort once with sorted_sort / sorted_sort_by_key after bulk construction
//   - mutate only via sorted_insert / sorted_try_insert / sorted_erase /
//     sorted_erase_at (each is O(n) for the shift)
//   - never insert/erase via vector::push_back or vector::erase directly
//     after sorting; doing so invalidates the sorted invariant.
//
// Lookup: sorted_find(v, key, key_of) returns pointer to the element, or
// nullptr. O(log n) with lower_bound. sorted_contains is the boolean form.
// sorted_lower_bound / sorted_upper_bound / sorted_equal_range return
// positions (ptrdiff_t indices), mirroring std::map / std::set ordered
// queries.
//
// Duplicates: sorted_insert allows them (inserts at the correct sorted
// position, even if a duplicate key exists). sorted_try_insert disallows
// them (returns existing on duplicate). The caller picks the policy.
//
// Style: snake_case, One True Brace Style, template params T / Compare /
// KeyOf / Key.

#include "bc_vector.h"
#include "bc_pair.h"

#include <stddef.h>
#include <algorithm>
#include <utility>

namespace df {

template <typename T, typename Compare>
void sorted_sort(vector<T>& v, Compare less) {
    std::sort(v.begin(), v.end(), less);
}

template <typename T, typename KeyOf>
void sorted_sort_by_key(vector<T>& v, KeyOf key_of) {
    std::sort(v.begin(), v.end(),
        [&](const T& a, const T& b) { return key_of(a) < key_of(b); });
}

template <typename T, typename Key, typename KeyOf>
T* sorted_find(vector<T>& v, const Key& key, KeyOf key_of) {
    auto it = std::lower_bound(v.begin(), v.end(), key,
        [&](const T& element, const Key& k) { return key_of(element) < k; });
    if (it != v.end() && !(key < key_of(*it))) {
        return &*it;
    }
    return nullptr;
}

template <typename T, typename Key, typename KeyOf>
const T* sorted_find(const vector<T>& v, const Key& key, KeyOf key_of) {
    auto it = std::lower_bound(v.begin(), v.end(), key,
        [&](const T& element, const Key& k) { return key_of(element) < k; });
    if (it != v.end() && !(key < key_of(*it))) {
        return &*it;
    }
    return nullptr;
}

template <typename T, typename Key, typename KeyOf>
bool sorted_contains(const vector<T>& v, const Key& key, KeyOf key_of) {
    return sorted_find(v, key, key_of) != nullptr;
}

template <typename T, typename Key, typename KeyOf>
ptrdiff_t sorted_lower_bound(const vector<T>& v, const Key& key, KeyOf key_of) {
    auto it = std::lower_bound(v.begin(), v.end(), key,
        [&](const T& element, const Key& k) { return key_of(element) < k; });
    return static_cast<ptrdiff_t>(it - v.begin());
}

template <typename T, typename Key, typename KeyOf>
ptrdiff_t sorted_upper_bound(const vector<T>& v, const Key& key, KeyOf key_of) {
    auto it = std::upper_bound(v.begin(), v.end(), key,
        [&](const Key& k, const T& element) { return k < key_of(element); });
    return static_cast<ptrdiff_t>(it - v.begin());
}

template <typename T, typename Key, typename KeyOf>
pair<ptrdiff_t, ptrdiff_t> sorted_equal_range(const vector<T>& v, const Key& key, KeyOf key_of) {
    return { sorted_lower_bound(v, key, key_of),
             sorted_upper_bound(v, key, key_of) };
}

template <typename T, typename KeyOf>
T* sorted_insert(vector<T>& v, T value, KeyOf key_of) {
    auto it = std::lower_bound(v.begin(), v.end(), key_of(value),
        [&](const T& element, const auto& k) { return key_of(element) < k; });
    const ptrdiff_t pos = static_cast<ptrdiff_t>(it - v.begin());
    v.insert(pos, std::move(value));
    return &v[pos];
}

template <typename T>
struct sorted_insert_result {
    T* value;
    bool inserted;
};

template <typename T, typename KeyOf>
sorted_insert_result<T> sorted_try_insert(vector<T>& v, T value, KeyOf key_of) {
    auto it = std::lower_bound(v.begin(), v.end(), key_of(value),
        [&](const T& element, const auto& k) { return key_of(element) < k; });
    const ptrdiff_t pos = static_cast<ptrdiff_t>(it - v.begin());
    if (it != v.end() && !(key_of(value) < key_of(*it))) {
        return { &*it, false };
    }
    v.insert(pos, std::move(value));
    return { &v[pos], true };
}

template <typename T, typename Key, typename KeyOf>
bool sorted_erase(vector<T>& v, const Key& key, KeyOf key_of) {
    T* found = sorted_find(v, key, key_of);
    if (found == nullptr) {
        return false;
    }
    const ptrdiff_t pos = found - v.data();
    v.erase(pos);
    return true;
}

template <typename T>
void sorted_erase_at(vector<T>& v, ptrdiff_t pos) {
    v.erase(pos);
}

} // namespace df