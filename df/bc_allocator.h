#pragma once

// bc_allocator.h
//
// Hybrid pool allocator + linear arena for mid-2000s arena shooters.
//
// Modes:
//   - Pooled : a hybrid_pool_allocator is registered for this thread via
//              set_active_pool(). Containers route through it, and the pool
//              draws chunks from the systems slab (bc_systems_slab.h) with a
//              per-chunk malloc fallback.
//   - Fallback : no pool registered for this thread. Containers route to
//                aligned_alloc_raw / aligned_free_raw (system heap).
// Containers always call df::allocate / df::deallocate; they never touch
// the pool or slab directly, so they work in both modes without changes.
//
// 11 size classes: 32, 64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048.
// Allocations > 2048 bypass to aligned_alloc_raw.
//
// Style: snake_case, One True Brace Style.

#include "bc_raw_alloc.h"
#include "bc_systems_slab.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <new>

namespace df {

class hybrid_pool_allocator {
public:
    static constexpr size_t MAX_POOL_SIZE = 2048;
    static constexpr size_t BLOCK_ALIGN   = 64;
    static constexpr size_t NUM_CLASSES   = 11;
    static constexpr size_t HEADER        = 64;

    hybrid_pool_allocator() noexcept : hybrid_pool_allocator(true) {}

    explicit hybrid_pool_allocator(bool use_slab) noexcept : use_slab_(use_slab) {
        const size_t sizes[NUM_CLASSES] = {
            32, 64, 128,
            192, 256, 384, 512,
            768, 1024, 1536, 2048
        };
        for (size_t i = 0; i < NUM_CLASSES; ++i) {
            buckets_[i].block_size = sizes[i];
            buckets_[i].free_head  = nullptr;
        }
    }

    hybrid_pool_allocator(const hybrid_pool_allocator&) = delete;
    hybrid_pool_allocator& operator=(const hybrid_pool_allocator&) = delete;
    hybrid_pool_allocator(hybrid_pool_allocator&&) = delete;
    hybrid_pool_allocator& operator=(hybrid_pool_allocator&&) = delete;

    ~hybrid_pool_allocator() {
        chunk_record* c = chunk_head_;
        while (c != nullptr) {
            chunk_record* next = c->next;
            if (c->from_slab) {
                if (c->page_size == systems_slab::PAGE_4K) {
                    global_systems_slab().release_4k(c);
                } else {
                    global_systems_slab().release_16k(c);
                }
            } else {
                aligned_free_raw(c);
            }
            c = next;
        }
        chunk_head_ = nullptr;
    }

    void* allocate(size_t size) noexcept {
        if (size == 0) {
            size = 1;
        }
        if (size > MAX_POOL_SIZE) {
            return aligned_alloc_raw(size, BLOCK_ALIGN);
        }
        const size_t idx = bucket_index_for(size);
        bucket& b = buckets_[idx];

        if (b.free_head == nullptr) {
            if (!grow_bucket(b)) {
                return nullptr;
            }
        }

        memory_node* n = b.free_head;
        b.free_head = n->next;
        return static_cast<void*>(n);
    }

    void deallocate(void* p, size_t size) noexcept {
        if (p == nullptr) {
            return;
        }
        if (size == 0) {
            size = 1;
        }
        if (size > MAX_POOL_SIZE) {
            aligned_free_raw(p);
            return;
        }
        const size_t idx = bucket_index_for(size);
        bucket& b = buckets_[idx];

        memory_node* n = static_cast<memory_node*>(p);
        n->next = b.free_head;
        b.free_head = n;
    }

    void* reallocate(void* p, size_t old_size, size_t new_size) noexcept {
        if (p == nullptr) {
            return allocate(new_size);
        }
        if (old_size == 0) {
            old_size = 1;
        }
        if (new_size == 0) {
            new_size = 1;
        }

        const bool old_pooled = (old_size <= MAX_POOL_SIZE);
        const bool new_pooled = (new_size <= MAX_POOL_SIZE);

        if (old_pooled && new_pooled) {
            const size_t old_idx = bucket_index_for(old_size);
            const size_t new_idx = bucket_index_for(new_size);
            if (old_idx == new_idx) {
                return p;
            }
        }

        void* fresh = allocate(new_size);
        if (fresh == nullptr) {
            return nullptr;
        }
        const size_t copy_n = (old_size < new_size) ? old_size : new_size;
        memcpy(fresh, p, copy_n);
        deallocate(p, old_size);
        return fresh;
    }

private:
    struct memory_node {
        memory_node* next;
    };

    struct bucket {
        size_t       block_size;
        memory_node* free_head;
    };

    struct chunk_record {
        chunk_record*  next;
        unsigned char* payload;
        size_t         page_size;
        bool           from_slab;
    };

    bucket buckets_[NUM_CLASSES] = {};
    chunk_record* chunk_head_ = nullptr;
    bool use_slab_;

    static constexpr size_t bucket_index_for(size_t size) noexcept {
        if (size <= 32)   return 0;
        if (size <= 64)   return 1;
        if (size <= 128)  return 2;
        if (size <= 192)  return 3;
        if (size <= 256)  return 4;
        if (size <= 384)  return 5;
        if (size <= 512)  return 6;
        if (size <= 768)  return 7;
        if (size <= 1024) return 8;
        if (size <= 1536) return 9;
        return 10;
    }

    static constexpr size_t chunk_size_for(size_t block_size) noexcept {
        return (block_size >= 768) ? systems_slab::PAGE_16K : systems_slab::PAGE_4K;
    }

    bool grow_bucket(bucket& b) noexcept {
        const size_t chunk_size = chunk_size_for(b.block_size);
        const size_t stride     = align_up(b.block_size, BLOCK_ALIGN);

        void* raw = nullptr;
        bool from_slab = false;
        if (use_slab_) {
            if (chunk_size == systems_slab::PAGE_4K) {
                raw = global_systems_slab().allocate_4k();
                from_slab = (raw != nullptr);
            } else {
                raw = global_systems_slab().allocate_16k();
                from_slab = (raw != nullptr);
            }
        }
        if (raw == nullptr) {
            raw = aligned_alloc_raw(chunk_size, BLOCK_ALIGN);
            if (raw == nullptr) {
                return false;
            }
        }

        chunk_record* rec = static_cast<chunk_record*>(raw);
        rec->payload   = static_cast<unsigned char*>(raw) + HEADER;
        rec->page_size = chunk_size;
        rec->from_slab = from_slab;
        rec->next      = chunk_head_;
        chunk_head_    = rec;

        unsigned char* base = rec->payload;
        const size_t count = (chunk_size - HEADER) / stride;
        assert(count > 0);

        memory_node* head = nullptr;
        for (size_t i = 0; i < count; ++i) {
            memory_node* n = reinterpret_cast<memory_node*>(base + i * stride);
            n->next = head;
            head = n;
        }
        b.free_head = head;
        return true;
    }
};

inline hybrid_pool_allocator*& tls_active_pool() noexcept {
    thread_local hybrid_pool_allocator* p = nullptr;
    return p;
}

inline void set_active_pool(hybrid_pool_allocator* p) noexcept {
    tls_active_pool() = p;
}

inline hybrid_pool_allocator* get_active_pool() noexcept {
    return tls_active_pool();
}

inline void* allocate(size_t size) noexcept {
    hybrid_pool_allocator* p = get_active_pool();
    if (p != nullptr) {
        return p->allocate(size);
    }
    return aligned_alloc_raw(size, hybrid_pool_allocator::BLOCK_ALIGN);
}

inline void deallocate(void* ptr, size_t size) noexcept {
    if (ptr == nullptr) {
        return;
    }
    hybrid_pool_allocator* p = get_active_pool();
    if (p != nullptr) {
        p->deallocate(ptr, size);
        return;
    }
    aligned_free_raw(ptr);
}

inline void* reallocate(void* ptr, size_t old_size, size_t new_size) noexcept {
    hybrid_pool_allocator* p = get_active_pool();
    if (p != nullptr) {
        return p->reallocate(ptr, old_size, new_size);
    }
    if (ptr == nullptr) {
        return aligned_alloc_raw(new_size, hybrid_pool_allocator::BLOCK_ALIGN);
    }
    void* fresh = aligned_alloc_raw(new_size, hybrid_pool_allocator::BLOCK_ALIGN);
    if (fresh == nullptr) {
        return nullptr;
    }
    const size_t copy_n = (old_size < new_size) ? old_size : new_size;
    memcpy(fresh, ptr, copy_n);
    aligned_free_raw(ptr);
    return fresh;
}

class linear_arena {
public:
    using checkpoint = ptrdiff_t;

    linear_arena() noexcept = default;

    explicit linear_arena(size_t capacity, size_t align = 64) noexcept {
        init(capacity, align);
    }

    linear_arena(const linear_arena&) = delete;
    linear_arena& operator=(const linear_arena&) = delete;

    ~linear_arena() {
        release();
    }

    bool init(size_t capacity, size_t align = 64) noexcept {
        assert(align != 0);
        assert((align & (align - 1)) == 0 && "align must be a power of two");
        release();
        void* buf = aligned_alloc_raw(capacity, align);
        if (buf == nullptr) {
            return false;
        }
        buffer_start_   = static_cast<unsigned char*>(buf);
        total_capacity_ = static_cast<ptrdiff_t>(capacity);
        offset_marker_  = 0;
        base_align_     = align;
        return true;
    }

    void release() noexcept {
        if (buffer_start_ != nullptr) {
            aligned_free_raw(buffer_start_);
            buffer_start_ = nullptr;
        }
        total_capacity_ = 0;
        offset_marker_  = 0;
        base_align_     = 0;
    }

    void* allocate(size_t size, size_t align = 0) noexcept {
        void* p = try_allocate(size, align);
        assert(p != nullptr && "linear_arena overflow");
        return p;
    }

    void* try_allocate(size_t size, size_t align = 0) noexcept {
        assert(buffer_start_ != nullptr && "arena not initialised");
        if (size == 0) {
            size = 1;
        }
        if (align == 0) {
            align = base_align_;
        }
        assert((align & (align - 1)) == 0 && "align must be a power of two");
        assert(align <= base_align_ && "per-call align must not exceed base align");

        const ptrdiff_t misalignment = offset_marker_ & (static_cast<ptrdiff_t>(align) - 1);
        const ptrdiff_t padding = misalignment ? (static_cast<ptrdiff_t>(align) - misalignment) : 0;
        const ptrdiff_t need = padding + static_cast<ptrdiff_t>(size);

        if (offset_marker_ + need > total_capacity_) {
            return nullptr;
        }

        offset_marker_ += padding;
        void* p = buffer_start_ + offset_marker_;
        offset_marker_ += static_cast<ptrdiff_t>(size);
        return p;
    }

    checkpoint get_checkpoint() const noexcept {
        return offset_marker_;
    }

    void rewind(checkpoint cp) noexcept {
        assert(cp >= 0 && cp <= offset_marker_ && "invalid checkpoint");
        offset_marker_ = cp;
    }

    void reset() noexcept {
        offset_marker_ = 0;
    }

    ptrdiff_t ssize() const noexcept {
        return offset_marker_;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(offset_marker_);
    }

    ptrdiff_t scapacity() const noexcept {
        return total_capacity_;
    }

    size_t capacity() const noexcept {
        return static_cast<size_t>(total_capacity_);
    }

    void* data() noexcept {
        return buffer_start_;
    }

    const void* data() const noexcept {
        return buffer_start_;
    }

private:
    unsigned char* buffer_start_   = nullptr;
    ptrdiff_t      offset_marker_  = 0;
    ptrdiff_t      total_capacity_ = 0;
    size_t         base_align_     = 0;
};

} // namespace df