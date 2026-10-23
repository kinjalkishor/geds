#pragma once

// bc_object_pool.h
//
// Typed fixed-size object pool. Same type, many objects, per-object
// lifetime. Backed by df::allocate (pool -> slab -> malloc fallback).
//
// Style: snake_case, One True Brace Style, template param T.

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>

namespace df {

template <typename T>
class object_pool {
public:
    static constexpr ptrdiff_t default_chunk_size = 256;

    object_pool() noexcept = default;

    explicit object_pool(ptrdiff_t chunk_size) noexcept
        : chunk_size_(chunk_size > 0 ? chunk_size : default_chunk_size) {}

    object_pool(const object_pool&) = delete;
    object_pool& operator=(const object_pool&) = delete;

    object_pool(object_pool&& other) noexcept
        : chunks_(other.chunks_),
          chunk_count_(other.chunk_count_),
          chunk_cap_(other.chunk_cap_),
          free_head_(other.free_head_),
          live_count_(other.live_count_),
          chunk_size_(other.chunk_size_) {
        other.chunks_ = nullptr;
        other.chunk_count_ = 0;
        other.chunk_cap_ = 0;
        other.free_head_ = nullptr;
        other.live_count_ = 0;
        other.chunk_size_ = default_chunk_size;
    }

    object_pool& operator=(object_pool&& other) noexcept {
        if (this != &other) {
            release();
            chunks_ = other.chunks_;
            chunk_count_ = other.chunk_count_;
            chunk_cap_ = other.chunk_cap_;
            free_head_ = other.free_head_;
            live_count_ = other.live_count_;
            chunk_size_ = other.chunk_size_;
            other.chunks_ = nullptr;
            other.chunk_count_ = 0;
            other.chunk_cap_ = 0;
            other.free_head_ = nullptr;
            other.live_count_ = 0;
            other.chunk_size_ = default_chunk_size;
        }
        return *this;
    }

    ~object_pool() {
        release();
    }

    T* allocate() noexcept {
        if (free_head_ == nullptr) {
            if (!grow()) {
                return nullptr;
            }
        }
        free_node* node = free_head_;
        free_head_ = node->next;
        ++live_count_;
        return reinterpret_cast<T*>(node);
    }

    template <typename... Args>
    T* acquire(Args&&... args) {
        T* slot = allocate();
        if (slot == nullptr) {
            return nullptr;
        }
        new (slot) T(std::forward<Args>(args)...);
        return slot;
    }

    void deallocate(T* p) noexcept {
        assert(p != nullptr);
        assert(live_count_ > 0);
        free_node* node = reinterpret_cast<free_node*>(p);
        node->next = free_head_;
        free_head_ = node;
        --live_count_;
    }

    void release(T* p) noexcept {
        assert(p != nullptr);
        p->~T();
        deallocate(p);
    }

    ptrdiff_t ssize() const noexcept {
        return live_count_;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(live_count_);
    }

    ptrdiff_t chunk_count() const noexcept {
        return chunk_count_;
    }

    ptrdiff_t chunk_capacity() const noexcept {
        return chunk_count_ * chunk_size_;
    }

    ptrdiff_t scapacity() const noexcept {
        return chunk_count_ * chunk_size_;
    }

    ptrdiff_t free_count() const noexcept {
        return chunk_count_ * chunk_size_ - live_count_;
    }

    bool empty() const noexcept {
        return live_count_ == 0;
    }

    ptrdiff_t slots_per_chunk() const noexcept {
        return chunk_size_;
    }

    void reset() noexcept {
        free_head_ = nullptr;
        live_count_ = 0;
        for (ptrdiff_t c = chunk_count_ - 1; c >= 0; --c) {
            unsigned char* base = chunks_[c];
            for (ptrdiff_t s = chunk_size_ - 1; s >= 0; --s) {
                free_node* node = reinterpret_cast<free_node*>(base + s * sizeof(T));
                node->next = free_head_;
                free_head_ = node;
            }
        }
    }

    void release() noexcept {
        for (ptrdiff_t c = 0; c < chunk_count_; ++c) {
            df::deallocate(chunks_[c], static_cast<size_t>(chunk_size_) * sizeof(T));
        }
        if (chunks_ != nullptr) {
            df::deallocate(chunks_, static_cast<size_t>(chunk_cap_) * sizeof(unsigned char*));
            chunks_ = nullptr;
        }
        chunk_count_ = 0;
        chunk_cap_ = 0;
        free_head_ = nullptr;
        live_count_ = 0;
    }

private:
    struct free_node {
        free_node* next;
    };

    static_assert(sizeof(free_node) <= sizeof(T),
                  "object_pool<T>: T must be at least as large as a pointer "
                  "(free-list node overlays T's storage)");

    unsigned char** chunks_ = nullptr;
    ptrdiff_t chunk_count_ = 0;
    ptrdiff_t chunk_cap_ = 0;
    free_node* free_head_ = nullptr;
    ptrdiff_t live_count_ = 0;
    ptrdiff_t chunk_size_ = default_chunk_size;

    bool grow() noexcept {
        if (chunk_count_ == chunk_cap_) {
            const ptrdiff_t new_cap = chunk_cap_ > 0 ? chunk_cap_ * 2 : 8;
            void* raw = df::allocate(static_cast<size_t>(new_cap) * sizeof(unsigned char*));
            if (raw == nullptr) {
                return false;
            }
            unsigned char** fresh = static_cast<unsigned char**>(raw);
            for (ptrdiff_t i = 0; i < chunk_count_; ++i) {
                fresh[i] = chunks_[i];
            }
            if (chunks_ != nullptr) {
                df::deallocate(chunks_, static_cast<size_t>(chunk_cap_) * sizeof(unsigned char*));
            }
            chunks_ = fresh;
            chunk_cap_ = new_cap;
        }

        void* raw_chunk = df::allocate(static_cast<size_t>(chunk_size_) * sizeof(T));
        if (raw_chunk == nullptr) {
            return false;
        }
        unsigned char* chunk = static_cast<unsigned char*>(raw_chunk);
        chunks_[chunk_count_] = chunk;
        ++chunk_count_;

        for (ptrdiff_t s = chunk_size_ - 1; s >= 0; --s) {
            free_node* node = reinterpret_cast<free_node*>(chunk + s * sizeof(T));
            node->next = free_head_;
            free_head_ = node;
        }
        return true;
    }
};

} // namespace df