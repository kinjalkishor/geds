#pragma once

// bc_sparse_set.h
//
// Sparse set: dense array of T + sparse array mapping entity ID to dense
// index + reverse array mapping dense index to entity ID. O(1) insert,
// O(1) erase (via swap-and-pop), O(1) lookup, cache-friendly iteration.
//
// The canonical ECS component storage. Also useful for any "set of small
// integer IDs, iterate fast, membership check O(1)" workload.
//
// Entity ID type is templated (default uint32_t). Must be an unsigned
// integer type.
//
// Style: snake_case, One True Brace Style, template params T, Entity.

#include "bc_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

template <typename T, typename Entity = uint32_t>
class sparse_set {
    static_assert(std::is_unsigned_v<Entity>, "sparse_set: Entity must be an unsigned integer type");

public:
    using value_type      = T;
    using entity_type     = Entity;
    using size_type       = size_t;
    using difference_type = ptrdiff_t;

    static constexpr ptrdiff_t invalid_index = -1;

private:
    T*        dense_          = nullptr;
    Entity*   dense_entities_ = nullptr;
    ptrdiff_t dense_count_    = 0;
    ptrdiff_t dense_cap_      = 0;

    ptrdiff_t* sparse_        = nullptr;
    ptrdiff_t  sparse_cap_    = 0;

public:
    sparse_set() noexcept {}

    sparse_set(const sparse_set&) = delete;
    sparse_set& operator=(const sparse_set&) = delete;

    sparse_set(sparse_set&& other) noexcept {
        steal(other);
    }

    sparse_set& operator=(sparse_set&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~sparse_set() {
        destroy_all();
    }

    size_t    size()  const noexcept { return static_cast<size_t>(dense_count_); }
    ptrdiff_t ssize() const noexcept { return dense_count_; }
    bool      empty() const noexcept { return dense_count_ == 0; }

    bool contains(Entity e) const noexcept {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(e);
        if (idx < 0 || idx >= sparse_cap_) {
            return false;
        }
        return sparse_[idx] != invalid_index;
    }

    T* get(Entity e) noexcept {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(e);
        if (idx < 0 || idx >= sparse_cap_) {
            return nullptr;
        }
        const ptrdiff_t dense_idx = sparse_[idx];
        if (dense_idx == invalid_index) {
            return nullptr;
        }
        return &dense_[dense_idx];
    }
    const T* get(Entity e) const noexcept {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(e);
        if (idx < 0 || idx >= sparse_cap_) {
            return nullptr;
        }
        const ptrdiff_t dense_idx = sparse_[idx];
        if (dense_idx == invalid_index) {
            return nullptr;
        }
        return &dense_[dense_idx];
    }

    T& at(Entity e) {
        T* p = get(e);
        assert(p != nullptr && "sparse_set::at entity not present");
        return *p;
    }
    const T& at(Entity e) const {
        const T* p = get(e);
        assert(p != nullptr && "sparse_set::at entity not present");
        return *p;
    }

    bool insert(Entity e, const T& v) {
        return do_insert(e, v);
    }
    bool insert(Entity e, T&& v) {
        return do_insert(e, std::move(v));
    }

    template <typename... Args>
    bool emplace(Entity e, Args&&... args) {
        const ptrdiff_t eidx = static_cast<ptrdiff_t>(e);
        if (eidx >= sparse_cap_) {
            ensure_sparse_cap(eidx + 1);
        }
        const ptrdiff_t existing = sparse_[eidx];
        if (existing != invalid_index) {
            return false;
        }
        const ptrdiff_t dense_idx = append_dense_slot(e);
        new (&dense_[dense_idx]) T(std::forward<Args>(args)...);
        return true;
    }

    bool erase(Entity e) {
        const ptrdiff_t idx = static_cast<ptrdiff_t>(e);
        if (idx < 0 || idx >= sparse_cap_) {
            return false;
        }
        const ptrdiff_t dense_idx = sparse_[idx];
        if (dense_idx == invalid_index) {
            return false;
        }
        const ptrdiff_t last = dense_count_ - 1;
        if (dense_idx != last) {
            dense_[dense_idx] = std::move(dense_[last]);
            const Entity moved_entity = dense_entities_[last];
            dense_entities_[dense_idx] = moved_entity;
            sparse_[static_cast<ptrdiff_t>(moved_entity)] = dense_idx;
        }
        dense_[last].~T();
        dense_entities_[last] = static_cast<Entity>(-1);
        sparse_[idx] = invalid_index;
        --dense_count_;
        return true;
    }

    void clear() noexcept {
        for (ptrdiff_t i = 0; i < dense_count_; ++i) {
            sparse_[static_cast<ptrdiff_t>(dense_entities_[i])] = invalid_index;
            dense_[i].~T();
        }
        dense_count_ = 0;
    }

    void reserve_entities(Entity max_entity) {
        ensure_sparse_cap(static_cast<ptrdiff_t>(max_entity) + 1);
    }

    void reserve_dense(ptrdiff_t n) {
        if (n > dense_cap_) {
            grow_dense(n);
        }
    }

    void swap(sparse_set& other) noexcept {
        std::swap(dense_, other.dense_);
        std::swap(dense_entities_, other.dense_entities_);
        std::swap(dense_count_, other.dense_count_);
        std::swap(dense_cap_, other.dense_cap_);
        std::swap(sparse_, other.sparse_);
        std::swap(sparse_cap_, other.sparse_cap_);
    }

    T*       data()       noexcept { return dense_; }
    const T* data() const noexcept { return dense_; }

    T*       begin()       noexcept { return dense_; }
    T*       end()         noexcept { return dense_ + dense_count_; }
    const T* begin() const noexcept { return dense_; }
    const T* end()   const noexcept { return dense_ + dense_count_; }
    const T* cbegin() const noexcept { return dense_; }
    const T* cend()   const noexcept { return dense_ + dense_count_; }

    Entity*       entities_begin()       noexcept { return dense_entities_; }
    Entity*       entities_end()         noexcept { return dense_entities_ + dense_count_; }
    const Entity* entities_begin() const noexcept { return dense_entities_; }
    const Entity* entities_end()   const noexcept { return dense_entities_ + dense_count_; }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (ptrdiff_t i = 0; i < dense_count_; ++i) {
            fn(dense_entities_[i], dense_[i]);
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (ptrdiff_t i = 0; i < dense_count_; ++i) {
            fn(dense_entities_[i], dense_[i]);
        }
    }

private:
    ptrdiff_t append_dense_slot(Entity e) {
        if (dense_count_ == dense_cap_) {
            grow_dense(dense_cap_ > 0 ? dense_cap_ + dense_cap_ / 2 + 8 : 16);
        }
        const ptrdiff_t dense_idx = dense_count_;
        const ptrdiff_t eidx = static_cast<ptrdiff_t>(e);
        if (eidx >= sparse_cap_) {
            ensure_sparse_cap(eidx + 1);
        }
        sparse_[eidx] = dense_idx;
        dense_entities_[dense_idx] = e;
        ++dense_count_;
        return dense_idx;
    }

    template <typename V>
    bool do_insert(Entity e, V&& v) {
        const ptrdiff_t eidx = static_cast<ptrdiff_t>(e);
        if (eidx >= sparse_cap_) {
            ensure_sparse_cap(eidx + 1);
        }
        const ptrdiff_t existing = sparse_[eidx];
        if (existing != invalid_index) {
            dense_[existing] = std::forward<V>(v);
            return false;
        }
        const ptrdiff_t dense_idx = append_dense_slot(e);
        new (&dense_[dense_idx]) T(std::forward<V>(v));
        return true;
    }

    void grow_dense(ptrdiff_t new_cap) {
        void* raw_values = df::allocate(static_cast<size_t>(new_cap) * sizeof(T));
        assert(raw_values != nullptr && "sparse_set: dense values allocation failed");
        T* fresh_values = static_cast<T*>(raw_values);

        void* raw_entities = df::allocate(static_cast<size_t>(new_cap) * sizeof(Entity));
        assert(raw_entities != nullptr && "sparse_set: dense entities allocation failed");
        Entity* fresh_entities = static_cast<Entity*>(raw_entities);

        if (dense_count_ > 0) {
            memcpy(fresh_entities, dense_entities_, static_cast<size_t>(dense_count_) * sizeof(Entity));
            for (ptrdiff_t i = 0; i < dense_count_; ++i) {
                new (&fresh_values[i]) T(std::move_if_noexcept(dense_[i]));
                dense_[i].~T();
            }
        }
        if (dense_ != nullptr) {
            df::deallocate(dense_, static_cast<size_t>(dense_cap_) * sizeof(T));
        }
        if (dense_entities_ != nullptr) {
            df::deallocate(dense_entities_, static_cast<size_t>(dense_cap_) * sizeof(Entity));
        }
        dense_ = fresh_values;
        dense_entities_ = fresh_entities;
        dense_cap_ = new_cap;
    }

    void ensure_sparse_cap(ptrdiff_t new_cap) {
        if (new_cap <= sparse_cap_) {
            return;
        }
        ptrdiff_t target = sparse_cap_ > 0 ? sparse_cap_ : 16;
        while (target < new_cap) {
            target = target + target / 2 + 8;
        }
        void* raw = df::allocate(static_cast<size_t>(target) * sizeof(ptrdiff_t));
        assert(raw != nullptr && "sparse_set: sparse allocation failed");
        ptrdiff_t* fresh = static_cast<ptrdiff_t*>(raw);
        for (ptrdiff_t i = 0; i < target; ++i) {
            fresh[i] = invalid_index;
        }
        if (sparse_cap_ > 0) {
            memcpy(fresh, sparse_, static_cast<size_t>(sparse_cap_) * sizeof(ptrdiff_t));
        }
        if (sparse_ != nullptr) {
            df::deallocate(sparse_, static_cast<size_t>(sparse_cap_) * sizeof(ptrdiff_t));
        }
        sparse_ = fresh;
        sparse_cap_ = target;
    }

    void destroy_all() noexcept {
        if (dense_ != nullptr) {
            for (ptrdiff_t i = 0; i < dense_count_; ++i) {
                dense_[i].~T();
            }
            df::deallocate(dense_, static_cast<size_t>(dense_cap_) * sizeof(T));
            dense_ = nullptr;
        }
        if (dense_entities_ != nullptr) {
            df::deallocate(dense_entities_, static_cast<size_t>(dense_cap_) * sizeof(Entity));
            dense_entities_ = nullptr;
        }
        if (sparse_ != nullptr) {
            df::deallocate(sparse_, static_cast<size_t>(sparse_cap_) * sizeof(ptrdiff_t));
            sparse_ = nullptr;
        }
        dense_count_ = 0;
        dense_cap_ = 0;
        sparse_cap_ = 0;
    }

    void steal(sparse_set& other) noexcept {
        dense_ = other.dense_;
        dense_entities_ = other.dense_entities_;
        dense_count_ = other.dense_count_;
        dense_cap_ = other.dense_cap_;
        sparse_ = other.sparse_;
        sparse_cap_ = other.sparse_cap_;

        other.dense_ = nullptr;
        other.dense_entities_ = nullptr;
        other.dense_count_ = 0;
        other.dense_cap_ = 0;
        other.sparse_ = nullptr;
        other.sparse_cap_ = 0;
    }
};

template <typename T, typename Entity>
inline void swap(sparse_set<T, Entity>& a, sparse_set<T, Entity>& b) noexcept {
    a.swap(b);
}

} // namespace df