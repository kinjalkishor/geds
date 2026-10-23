#pragma once

// bc_lru_cache.h
//
// Fixed-capacity LRU cache. On put() past capacity, evicts the
// least-recently-used entry. get() and put() both promote the touched entry
// to most-recently-used.
//
// Model:
//   bucket array: open-addressed hash from key to node, sized to >= 2x
//                 capacity so it never rehashes
//   recency list: doubly-linked list, MRU at head, LRU at tail
//   Node:         { K key; V value; Node* prev; Node* next; Node* bucket_next; }
//
// Nodes are pool-backed (df::allocate / df::deallocate). O(1) get / put /
// erase. No per-operation allocation on hit.
//
// Style: snake_case, One True Brace Style, template params K, V.

#include "bc_allocator.h"
#include "bc_hash.h"
#include "bc_pair.h"

#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <new>
#include <utility>

namespace df {

template <typename K, typename V>
class lru_cache {
public:
    using key_type        = K;
    using mapped_type     = V;
    using value_type      = df::pair<K, V>;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;

private:
    struct node {
        K     key;
        V     value;
        node* prev;
        node* next;
        node* bucket_next;
    };

    ptrdiff_t capacity_ = 0;
    ptrdiff_t size_     = 0;
    node*     head_     = nullptr;
    node*     tail_     = nullptr;

    node** buckets_      = nullptr;
    ptrdiff_t bucket_count_ = 0;
    ptrdiff_t bucket_mask_  = 0;

public:
    explicit lru_cache(ptrdiff_t capacity) {
        assert(capacity > 0);
        capacity_ = capacity;
        ensure_buckets();
    }

    lru_cache(const lru_cache&) = delete;
    lru_cache& operator=(const lru_cache&) = delete;

    lru_cache(lru_cache&& other) noexcept {
        steal(other);
    }

    lru_cache& operator=(lru_cache&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~lru_cache() {
        destroy_all();
    }

    size_t    size()     const noexcept { return static_cast<size_t>(size_); }
    ptrdiff_t ssize()    const noexcept { return size_; }
    ptrdiff_t scapacity() const noexcept { return capacity_; }
    size_t    capacity() const noexcept { return static_cast<size_t>(capacity_); }
    bool      empty()    const noexcept { return size_ == 0; }

    V* get(const K& key) {
        node* n = find_node(key);
        if (n == nullptr) {
            return nullptr;
        }
        move_to_front(n);
        return &n->value;
    }
    const V* get(const K& key) const {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }

    V* peek(const K& key) {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }
    const V* peek(const K& key) const {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }

    bool contains(const K& key) const {
        return find_node(key) != nullptr;
    }

    void put(const K& key, const V& value) {
        do_put(key, value);
    }
    void put(K&& key, V&& value) {
        do_put(std::move(key), std::move(value));
    }

    bool erase(const K& key) {
        node* n = find_node(key);
        if (n == nullptr) {
            return false;
        }
        unlink(n);
        remove_from_bucket(n);
        destroy_node(n);
        --size_;
        return true;
    }

    void clear() noexcept {
        node* n = head_;
        while (n != nullptr) {
            node* next = n->next;
            destroy_node(n);
            n = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
        if (buckets_ != nullptr) {
            memset(buckets_, 0, static_cast<size_t>(bucket_count_) * sizeof(node*));
        }
    }

    void swap(lru_cache& other) noexcept {
        std::swap(capacity_, other.capacity_);
        std::swap(size_, other.size_);
        std::swap(head_, other.head_);
        std::swap(tail_, other.tail_);
        std::swap(buckets_, other.buckets_);
        std::swap(bucket_count_, other.bucket_count_);
        std::swap(bucket_mask_, other.bucket_mask_);
    }

private:
    static uint64_t hash_key(const K& key) {
        return wy::hash_of(key, 0);
    }
    static bool key_eq(const K& a, const K& b) {
        return a == b;
    }

    node* alloc_node() {
        void* raw = df::allocate(sizeof(node));
        assert(raw != nullptr && "lru_cache: node allocation failed");
        return static_cast<node*>(raw);
    }

    void destroy_node(node* n) {
        n->key.~K();
        n->value.~V();
        df::deallocate(n, sizeof(node));
    }

    void destroy_all() noexcept {
        clear();
        if (buckets_ != nullptr) {
            df::deallocate(buckets_, static_cast<size_t>(bucket_count_) * sizeof(node*));
            buckets_ = nullptr;
        }
        bucket_count_ = 0;
        bucket_mask_ = 0;
        capacity_ = 0;
    }

    void steal(lru_cache& other) noexcept {
        capacity_ = other.capacity_;
        size_ = other.size_;
        head_ = other.head_;
        tail_ = other.tail_;
        buckets_ = other.buckets_;
        bucket_count_ = other.bucket_count_;
        bucket_mask_ = other.bucket_mask_;

        other.capacity_ = 0;
        other.size_ = 0;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.buckets_ = nullptr;
        other.bucket_count_ = 0;
        other.bucket_mask_ = 0;
    }

    void ensure_buckets() {
        if (buckets_ != nullptr) {
            return;
        }
        ptrdiff_t n = 16;
        while (n < capacity_ * 2) {
            n <<= 1;
        }
        void* raw = df::allocate(static_cast<size_t>(n) * sizeof(node*));
        assert(raw != nullptr && "lru_cache: bucket allocation failed");
        buckets_ = static_cast<node**>(raw);
        memset(buckets_, 0, static_cast<size_t>(n) * sizeof(node*));
        bucket_count_ = n;
        bucket_mask_ = n - 1;
    }

    node* find_node(const K& key) const {
        if (buckets_ == nullptr) {
            return nullptr;
        }
        const ptrdiff_t idx = static_cast<ptrdiff_t>(hash_key(key) & static_cast<uint64_t>(bucket_mask_));
        node* n = buckets_[idx];
        while (n != nullptr) {
            if (key_eq(n->key, key)) {
                return n;
            }
            n = n->bucket_next;
        }
        return nullptr;
    }

    void unlink(node* n) {
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            head_ = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        } else {
            tail_ = n->prev;
        }
        n->prev = nullptr;
        n->next = nullptr;
    }

    void push_front(node* n) {
        n->prev = nullptr;
        n->next = head_;
        if (head_ != nullptr) {
            head_->prev = n;
        }
        head_ = n;
        if (tail_ == nullptr) {
            tail_ = n;
        }
    }

    void move_to_front(node* n) {
        if (head_ == n) {
            return;
        }
        unlink(n);
        push_front(n);
    }

    void insert_into_bucket(node* n) {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(hash_key(n->key) & static_cast<uint64_t>(bucket_mask_));
        n->bucket_next = buckets_[idx];
        buckets_[idx] = n;
    }

    void remove_from_bucket(node* n) {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(hash_key(n->key) & static_cast<uint64_t>(bucket_mask_));
        node* cur = buckets_[idx];
        node* prev = nullptr;
        while (cur != nullptr) {
            if (cur == n) {
                if (prev != nullptr) {
                    prev->bucket_next = cur->bucket_next;
                } else {
                    buckets_[idx] = cur->bucket_next;
                }
                return;
            }
            prev = cur;
            cur = cur->bucket_next;
        }
    }

    void evict_lru() {
        assert(tail_ != nullptr);
        node* n = tail_;
        unlink(n);
        remove_from_bucket(n);
        destroy_node(n);
        --size_;
    }

    template <typename KK, typename VV>
    void do_put(KK&& key, VV&& value) {
        node* existing = find_node(key);
        if (existing != nullptr) {
            existing->value = std::forward<VV>(value);
            move_to_front(existing);
            return;
        }
        node* n = alloc_node();
        new (&n->key) K(std::forward<KK>(key));
        new (&n->value) V(std::forward<VV>(value));
        n->prev = nullptr;
        n->next = nullptr;
        n->bucket_next = nullptr;
        insert_into_bucket(n);
        push_front(n);
        ++size_;
        if (size_ > capacity_) {
            evict_lru();
        }
    }
};

template <typename K, typename V>
inline void swap(lru_cache<K, V>& a, lru_cache<K, V>& b) noexcept {
    a.swap(b);
}

} // namespace df