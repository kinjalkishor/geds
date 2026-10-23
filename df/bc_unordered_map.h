#pragma once

// bc_unordered_map.h
//
// Chained hash map. Node payload is pair<const K, V> for map, K for set.
// The const K enforces the map's hash invariant: mutating a key through an
// iterator would put the node in the wrong bucket. Iterator returns
// pair<const K, V>& (map) or const K& (set). Matches std::unordered_map /
// std::unordered_set semantics.
//
// Key properties:
//   - Iterator and reference stability on insert (nodes never move)
//   - Erase invalidates only the erased iterator
//   - Const keys through iterators (no mutable-key footgun)
//   - count() returns size_t, matching std::unordered_map::count
//
// Hasher: wy::hash_of<K> (wy::hash_cstr for const char* / char*).
// Pool-backed via df::allocate / df::deallocate.
//
// Style: snake_case, One True Brace Style, template params K, V, Fn.

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
struct unordered_node_traits {
    using value_type = pair<const K, V>;

    template <typename KK, typename VV>
    static void construct(value_type* at, KK&& k, VV&& v) {
        new (&at->first)  const K(std::forward<KK>(k));
        new (&at->second) V(std::forward<VV>(v));
    }
    static void construct_default(value_type* at, const K& k) {
        new (&at->first)  const K(k);
        new (&at->second) V();
    }
    static void destroy(value_type* at) noexcept {
        at->second.~V();
        at->first.~K();
    }
    static const K& key_of(const value_type& v) noexcept { return v.first; }
};

template <typename K>
struct unordered_node_traits<K, void> {
    using value_type = K;

    static void construct(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void construct(value_type* at, K&& k) {
        new (at) K(std::move(k));
    }
    static void destroy(value_type* at) noexcept {
        at->~K();
    }
    static const K& key_of(const value_type& v) noexcept { return v; }
};

} // namespace detail

template <typename K, typename V>
class unordered_map {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = typename detail::unordered_node_traits<K, V>::value_type;
    using size_type   = size_t;

    static constexpr bool is_set_v = std::is_void_v<V>;

private:
    using traits = detail::unordered_node_traits<K, V>;

    struct node {
        value_type value;
        node*      next;
    };

public:
    template <bool IsConst>
    class iter_t {
        using node_ptr = std::conditional_t<IsConst, const node*, node*>;
        using value_ref = std::conditional_t<
            IsConst || is_set_v,
            const value_type&,
            value_type&>;
        using value_pp = std::conditional_t<
            IsConst || is_set_v,
            const value_type*,
            value_type*>;

        node_ptr cur_ = nullptr;

    public:
        using value_type        = unordered_map::value_type;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::forward_iterator_tag;

        iter_t() noexcept = default;
        explicit iter_t(node_ptr n) noexcept : cur_(n) {}

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept : cur_(other.cur_) {}

        reference operator*()  const noexcept { return cur_->value; }
        pointer   operator->() const noexcept { return &cur_->value; }

        iter_t& operator++() noexcept {
            cur_ = cur_->next;
            return *this;
        }
        iter_t operator++(int) noexcept {
            iter_t t(*this);
            ++(*this);
            return t;
        }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return cur_ == o.cur_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return cur_ != o.cur_; }

        node_ptr raw() const noexcept { return cur_; }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    unordered_map() noexcept
        : buckets_(nullptr), bucket_count_(0), mask_(0), len_(0) {}

    unordered_map(const unordered_map&) = delete;
    unordered_map& operator=(const unordered_map&) = delete;

    unordered_map(unordered_map&& other) noexcept
        : buckets_(other.buckets_),
          bucket_count_(other.bucket_count_),
          mask_(other.mask_),
          len_(other.len_) {
        other.buckets_ = nullptr;
        other.bucket_count_ = 0;
        other.mask_ = 0;
        other.len_ = 0;
    }

    unordered_map& operator=(unordered_map&& other) noexcept {
        if (this != &other) {
            destroy_all();
            buckets_ = other.buckets_;
            bucket_count_ = other.bucket_count_;
            mask_ = other.mask_;
            len_ = other.len_;
            other.buckets_ = nullptr;
            other.bucket_count_ = 0;
            other.mask_ = 0;
            other.len_ = 0;
        }
        return *this;
    }

    ~unordered_map() {
        destroy_all();
    }

    size_t    size()           const noexcept { return static_cast<size_t>(len_); }
    ptrdiff_t ssize()          const noexcept { return len_; }
    size_t    bucket_count()   const noexcept { return static_cast<size_t>(bucket_count_); }
    ptrdiff_t sbucket_count()  const noexcept { return bucket_count_; }
    bool      empty()          const noexcept { return len_ == 0; }

    void reserve(ptrdiff_t expected_len) {
        if (expected_len <= 0) {
            return;
        }
        const ptrdiff_t need = next_pow2((expected_len * 2) + 1);
        if (need <= bucket_count_) {
            return;
        }
        rehash_to(need);
    }

    void rehash(ptrdiff_t new_bucket_count) {
        if (new_bucket_count < 1) {
            new_bucket_count = 1;
        }
        rehash_to(next_pow2(new_bucket_count));
    }

    iterator begin() noexcept {
        iterator it(buckets_, bucket_count_, 0, nullptr);
        it.advance_to_next_nonempty();
        return it;
    }
    iterator end() noexcept {
        return iterator(buckets_, bucket_count_, bucket_count_, nullptr);
    }
    const_iterator begin() const noexcept {
        const_iterator it(buckets_, bucket_count_, 0, nullptr);
        it.advance_to_next_nonempty();
        return it;
    }
    const_iterator end() const noexcept {
        return const_iterator(buckets_, bucket_count_, bucket_count_, nullptr);
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    void insert_or_assign(const K& key, const V& value) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(key, value);
    }
    void insert_or_assign(K&& key, V&& value) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(std::move(key), std::move(value));
    }

    bool insert(const K& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        if (find_node(key) != nullptr) {
            return false;
        }
        ensure_bucket_room_for_one();
        add_node_key(key);
        return true;
    }
    bool insert(K&& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        if (find_node(key) != nullptr) {
            return false;
        }
        ensure_bucket_room_for_one();
        add_node_key(std::move(key));
        return true;
    }

    bool try_insert(const K& key, const V& value) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        if (find_node(key) != nullptr) {
            return false;
        }
        ensure_bucket_room_for_one();
        add_node_kv(key, value);
        return true;
    }
    bool try_insert(K&& key, V&& value) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        if (find_node(key) != nullptr) {
            return false;
        }
        ensure_bucket_room_for_one();
        add_node_kv(std::move(key), std::move(value));
        return true;
    }

    V& operator[](const K& key) {
        static_assert(!is_set_v, "operator[] is not available on a set");
        node* existing = find_node(key);
        if (existing != nullptr) {
            return existing->value.second;
        }
        ensure_bucket_room_for_one();
        node* fresh = add_node_default(key);
        return fresh->value.second;
    }

    bool erase(const K& key) {
        if (buckets_ == nullptr) {
            return false;
        }
        const ptrdiff_t idx =
            static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(mask_));
        node* prev = nullptr;
        node* cur = buckets_[idx];
        while (cur != nullptr) {
            if (key_equal(traits::key_of(cur->value), key)) {
                if (prev != nullptr) {
                    prev->next = cur->next;
                } else {
                    buckets_[idx] = cur->next;
                }
                destroy_node(cur);
                --len_;
                return true;
            }
            prev = cur;
            cur = cur->next;
        }
        return false;
    }

    void clear() noexcept {
        if (buckets_ == nullptr) {
            len_ = 0;
            return;
        }
        for (ptrdiff_t i = 0; i < bucket_count_; ++i) {
            node* n = buckets_[i];
            while (n != nullptr) {
                node* next = n->next;
                destroy_node(n);
                n = next;
            }
            buckets_[i] = nullptr;
        }
        len_ = 0;
    }

    void swap(unordered_map& other) noexcept {
        std::swap(buckets_, other.buckets_);
        std::swap(bucket_count_, other.bucket_count_);
        std::swap(mask_, other.mask_);
        std::swap(len_, other.len_);
    }

    value_type* find(const K& key) noexcept {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }
    const value_type* find(const K& key) const noexcept {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }

    bool contains(const K& key) const noexcept {
        return find_node(key) != nullptr;
    }

    size_t count(const K& key) const noexcept {
        return find_node(key) != nullptr ? 1 : 0;
    }

    V& at(const K& key) {
        static_assert(!is_set_v, "at() is not available on a set");
        node* n = find_node(key);
        assert(n != nullptr && "unordered_map::at key not found");
        return n->value.second;
    }
    const V& at(const K& key) const {
        static_assert(!is_set_v, "at() is not available on a set");
        node* n = find_node(key);
        assert(n != nullptr && "unordered_map::at key not found");
        return n->value.second;
    }

    V& get_or_insert(const K& key, const V& default_value) {
        static_assert(!is_set_v, "get_or_insert is not available on a set");
        node* existing = find_node(key);
        if (existing != nullptr) {
            return existing->value.second;
        }
        ensure_bucket_room_for_one();
        node* fresh = add_node_kv(key, default_value);
        return fresh->value.second;
    }

    template <typename Fn>
    void for_each(Fn&& fn) {
        if (buckets_ == nullptr) {
            return;
        }
        for (ptrdiff_t i = 0; i < bucket_count_; ++i) {
            node* n = buckets_[i];
            while (n != nullptr) {
                if constexpr (is_set_v) {
                    fn(traits::key_of(n->value));
                } else {
                    fn(n->value.first, n->value.second);
                }
                n = n->next;
            }
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        if (buckets_ == nullptr) {
            return;
        }
        for (ptrdiff_t i = 0; i < bucket_count_; ++i) {
            node* n = buckets_[i];
            while (n != nullptr) {
                if constexpr (is_set_v) {
                    fn(traits::key_of(n->value));
                } else {
                    fn(traits::key_of(n->value), n->value.second);
                }
                n = n->next;
            }
        }
    }

private:
    node**    buckets_;
    ptrdiff_t bucket_count_;
    ptrdiff_t mask_;
    ptrdiff_t len_;

    static uint64_t hash_key(const K& key) {
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

    static node* alloc_node() {
        void* raw = df::allocate(sizeof(node));
        assert(raw != nullptr && "unordered_map: node allocation failed");
        return static_cast<node*>(raw);
    }

    static void free_node(node* n) {
        df::deallocate(n, sizeof(node));
    }

    template <typename KK, typename VV>
    node* make_node_kv(KK&& key, VV&& value) {
        node* n = alloc_node();
        traits::construct(&n->value, std::forward<KK>(key), std::forward<VV>(value));
        n->next = nullptr;
        return n;
    }

    template <typename KK>
    node* make_node_key(KK&& key) {
        node* n = alloc_node();
        traits::construct(&n->value, std::forward<KK>(key));
        n->next = nullptr;
        return n;
    }

    node* make_node_default(const K& key) {
        node* n = alloc_node();
        traits::construct_default(&n->value, key);
        n->next = nullptr;
        return n;
    }

    static void destroy_node(node* n) {
        traits::destroy(&n->value);
        free_node(n);
    }

    static ptrdiff_t next_pow2(ptrdiff_t v) {
        ptrdiff_t p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    void allocate_bucket_array(ptrdiff_t count) {
        void* raw = df::allocate(static_cast<size_t>(count) * sizeof(node*));
        assert(raw != nullptr && "unordered_map: bucket allocation failed");
        buckets_ = static_cast<node**>(raw);
        bucket_count_ = count;
        mask_ = count - 1;
        memset(buckets_, 0, static_cast<size_t>(count) * sizeof(node*));
    }

    void ensure_bucket_room_for_one() {
        if (buckets_ == nullptr) {
            allocate_bucket_array(16);
            return;
        }
        if (len_ + 1 > bucket_count_) {
            rehash_to(bucket_count_ * 2);
        }
    }

    void rehash_to(ptrdiff_t new_bucket_count) {
        node** old_buckets = buckets_;
        const ptrdiff_t old_count = bucket_count_;

        node** fresh = static_cast<node**>(
            df::allocate(static_cast<size_t>(new_bucket_count) * sizeof(node*)));
        assert(fresh != nullptr && "unordered_map: rehash allocation failed");
        memset(fresh, 0, static_cast<size_t>(new_bucket_count) * sizeof(node*));
        const ptrdiff_t fresh_mask = new_bucket_count - 1;

        for (ptrdiff_t i = 0; i < old_count; ++i) {
            node* cur = old_buckets[i];
            while (cur != nullptr) {
                node* next = cur->next;
                const ptrdiff_t idx = static_cast<ptrdiff_t>(
                    hash_key(traits::key_of(cur->value)) & static_cast<uint64_t>(fresh_mask));
                cur->next = fresh[idx];
                fresh[idx] = cur;
                cur = next;
            }
        }

        if (old_buckets != nullptr) {
            df::deallocate(old_buckets, static_cast<size_t>(old_count) * sizeof(node*));
        }
        buckets_ = fresh;
        bucket_count_ = new_bucket_count;
        mask_ = fresh_mask;
    }

    node* find_node(const K& key) const noexcept {
        if (buckets_ == nullptr) {
            return nullptr;
        }
        const ptrdiff_t idx =
            static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(mask_));
        node* cur = buckets_[idx];
        while (cur != nullptr) {
            if (key_equal(traits::key_of(cur->value), key)) {
                return cur;
            }
            cur = cur->next;
        }
        return nullptr;
    }

    template <typename KK, typename VV>
    node* add_node_kv(KK&& key, VV&& value) {
        const ptrdiff_t idx =
            static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(mask_));
        node* fresh = make_node_kv(std::forward<KK>(key), std::forward<VV>(value));
        fresh->next = buckets_[idx];
        buckets_[idx] = fresh;
        ++len_;
        return fresh;
    }

    template <typename KK>
    node* add_node_key(KK&& key) {
        const ptrdiff_t idx =
            static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(mask_));
        node* fresh = make_node_key(std::forward<KK>(key));
        fresh->next = buckets_[idx];
        buckets_[idx] = fresh;
        ++len_;
        return fresh;
    }

    node* add_node_default(const K& key) {
        const ptrdiff_t idx =
            static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(mask_));
        node* fresh = make_node_default(key);
        fresh->next = buckets_[idx];
        buckets_[idx] = fresh;
        ++len_;
        return fresh;
    }

    template <typename KK, typename VV>
    void do_insert_or_assign(KK&& key, VV&& value) {
        node* existing = find_node(key);
        if (existing != nullptr) {
            existing->value.second = std::forward<VV>(value);
            return;
        }
        ensure_bucket_room_for_one();
        add_node_kv(std::forward<KK>(key), std::forward<VV>(value));
    }

    void destroy_all() noexcept {
        clear();
        if (buckets_ != nullptr) {
            df::deallocate(buckets_, static_cast<size_t>(bucket_count_) * sizeof(node*));
            buckets_ = nullptr;
        }
        bucket_count_ = 0;
        mask_ = 0;
    }
};

template <typename K, typename V>
inline void swap(unordered_map<K, V>& a, unordered_map<K, V>& b) noexcept {
    a.swap(b);
}

} // namespace df