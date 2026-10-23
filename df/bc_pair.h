#pragma once

// bc_pair.h
//
// Minimal tuple-like struct used by our containers.
// - public members first / second so structured bindings work:
//     for (auto& [k, v] : map) { ... }
// - no default-ctor body: members are left uninitialized when the pair is
//   default-constructed.
// - rule of zero: trivially copyable iff K and V are.
// - snake_case, One True Brace Style, template params K, V.

namespace df {

template <typename K, typename V>
struct pair {
    K first;
    V second;
};

template <typename K, typename V>
inline bool operator==(const pair<K, V>& a, const pair<K, V>& b) {
    return a.first == b.first && a.second == b.second;
}

template <typename K, typename V>
inline bool operator!=(const pair<K, V>& a, const pair<K, V>& b) {
    return !(a == b);
}

template <typename K, typename V>
inline bool operator<(const pair<K, V>& a, const pair<K, V>& b) {
    if (a.first < b.first) {
        return true;
    }
    if (b.first < a.first) {
        return false;
    }
    return a.second < b.second;
}

} // namespace df