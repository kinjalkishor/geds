#pragma once

// bc_systems_slab.h
//
// Two-tier systems slab: fixed-size page allocator for 4 KiB and 16 KiB pages.
// Backs hybrid_pool_allocator's chunk growth, eliminating per-chunk malloc
// calls from the steady state.
//
// Design:
//   - Two independent chains, one per page size (4 KiB, 16 KiB). No ratio
//     tuning; each chain grows only when its own freelist empties.
//   - Each chain grows by malloc'ing one BLOCK_SIZE (4 MiB) block, splitting
//     it into pages, and pushing those pages onto the freelist.
//   - Free pages are chained through a next pointer stored in the page itself.
//   - Lazy growth: no allocation at init(). The first allocate_4k() /
//     allocate_16k() call grows a block. preallocate() optionally warms the
//     slab up before gameplay.
//   - Freelist operations are protected by a spinlock (the slab is shared
//     across threads; pool buckets are thread-local and lock-free).
//
// Lifetime:
//   - Call df::systems_slab_init() before any hybrid_pool_allocator is created.
//   - Call df::systems_slab_shutdown() after all hybrid_pool_allocators have
//     been destroyed (so pools can return their chunks to a live slab).
//
// Style: snake_case, One True Brace Style.

#include "bc_raw_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <new>
#include <atomic>
#include <string.h>

namespace df {

class systems_slab {
public:
    static constexpr size_t BLOCK_SIZE = 4 * 1024 * 1024;
    static constexpr size_t PAGE_4K    = 4096;
    static constexpr size_t PAGE_16K   = 16384;
    static constexpr size_t HEADER     = 64;

    systems_slab() noexcept = default;

    systems_slab(const systems_slab&) = delete;
    systems_slab& operator=(const systems_slab&) = delete;

    ~systems_slab() noexcept {
        shutdown();
    }

    void init() noexcept {
    }

    bool preallocate(size_t pages_4k, size_t pages_16k) noexcept {
        lock();
        bool ok = true;
        size_t have_4k = count_free_4k_locked();
        while (ok && have_4k < pages_4k) {
            if (!grow_4k_chain_locked()) {
                ok = false;
            } else {
                have_4k = count_free_4k_locked();
            }
        }
        size_t have_16k = count_free_16k_locked();
        while (ok && have_16k < pages_16k) {
            if (!grow_16k_chain_locked()) {
                ok = false;
            } else {
                have_16k = count_free_16k_locked();
            }
        }
        unlock();
        return ok;
    }

    void* allocate_4k() noexcept {
        lock();
        if (free_4k_ == nullptr) {
            if (!grow_4k_chain_locked()) {
                unlock();
                return nullptr;
            }
        }
        slab_page* p = free_4k_;
        free_4k_ = p->next;
        unlock();
        return p;
    }

    void* allocate_16k() noexcept {
        lock();
        if (free_16k_ == nullptr) {
            if (!grow_16k_chain_locked()) {
                unlock();
                return nullptr;
            }
        }
        slab_page* p = free_16k_;
        free_16k_ = p->next;
        unlock();
        return p;
    }

    void release_4k(void* page) noexcept {
        if (page == nullptr) {
            return;
        }
        lock();
        slab_page* p = static_cast<slab_page*>(page);
        p->next = free_4k_;
        free_4k_ = p;
        unlock();
    }

    void release_16k(void* page) noexcept {
        if (page == nullptr) {
            return;
        }
        lock();
        slab_page* p = static_cast<slab_page*>(page);
        p->next = free_16k_;
        free_16k_ = p;
        unlock();
    }

    void shutdown() noexcept {
        lock();
        block_4k* b4 = blocks_4k_;
        blocks_4k_ = nullptr;
        free_4k_ = nullptr;

        block_16k* b16 = blocks_16k_;
        blocks_16k_ = nullptr;
        free_16k_ = nullptr;
        unlock();

        while (b4 != nullptr) {
            block_4k* next = b4->next;
            aligned_free_raw(b4);
            b4 = next;
        }
        while (b16 != nullptr) {
            block_16k* next = b16->next;
            aligned_free_raw(b16);
            b16 = next;
        }
    }

    size_t pages_free_4k() const noexcept {
        systems_slab* self = const_cast<systems_slab*>(this);
        self->lock();
        size_t n = count_free_4k_locked();
        self->unlock();
        return n;
    }

    size_t pages_free_16k() const noexcept {
        systems_slab* self = const_cast<systems_slab*>(this);
        self->lock();
        size_t n = count_free_16k_locked();
        self->unlock();
        return n;
    }

    size_t bytes_reserved() const noexcept {
        systems_slab* self = const_cast<systems_slab*>(this);
        self->lock();
        size_t total = 0;
        for (const block_4k* b = blocks_4k_; b != nullptr; b = b->next) {
            total += BLOCK_SIZE;
        }
        for (const block_16k* b = blocks_16k_; b != nullptr; b = b->next) {
            total += BLOCK_SIZE;
        }
        self->unlock();
        return total;
    }

private:
    struct slab_page {
        slab_page* next;
    };

    struct block_4k {
        block_4k* next;
        unsigned char* payload;
        size_t page_count;
    };

    struct block_16k {
        block_16k* next;
        unsigned char* payload;
        size_t page_count;
    };

    block_4k*        blocks_4k_  = nullptr;
    block_16k*       blocks_16k_ = nullptr;
    slab_page*       free_4k_    = nullptr;
    slab_page*       free_16k_   = nullptr;
    mutable std::atomic<bool> lock_{false};

    void lock() noexcept {
        while (lock_.exchange(true, std::memory_order_acquire)) {
            while (lock_.load(std::memory_order_relaxed)) {
            }
        }
    }

    void unlock() noexcept {
        lock_.store(false, std::memory_order_release);
    }

    bool grow_4k_chain_locked() noexcept {
        void* raw = aligned_alloc_raw(BLOCK_SIZE, PAGE_4K);
        if (raw == nullptr) {
            return false;
        }

        const size_t header = align_up(sizeof(block_4k), PAGE_4K);
        if (header >= BLOCK_SIZE) {
            aligned_free_raw(raw);
            return false;
        }

        block_4k* blk = static_cast<block_4k*>(raw);
        blk->payload = static_cast<unsigned char*>(raw) + header;
        blk->page_count = (BLOCK_SIZE - header) / PAGE_4K;
        blk->next = blocks_4k_;
        blocks_4k_ = blk;

        slab_page* head = free_4k_;
        for (size_t i = blk->page_count; i > 0; --i) {
            slab_page* page = reinterpret_cast<slab_page*>(
                blk->payload + (i - 1) * PAGE_4K);
            page->next = head;
            head = page;
        }
        free_4k_ = head;
        return true;
    }

    bool grow_16k_chain_locked() noexcept {
        void* raw = aligned_alloc_raw(BLOCK_SIZE, PAGE_16K);
        if (raw == nullptr) {
            return false;
        }

        const size_t header = align_up(sizeof(block_16k), PAGE_16K);
        if (header >= BLOCK_SIZE) {
            aligned_free_raw(raw);
            return false;
        }

        block_16k* blk = static_cast<block_16k*>(raw);
        blk->payload = static_cast<unsigned char*>(raw) + header;
        blk->page_count = (BLOCK_SIZE - header) / PAGE_16K;
        blk->next = blocks_16k_;
        blocks_16k_ = blk;

        slab_page* head = free_16k_;
        for (size_t i = blk->page_count; i > 0; --i) {
            slab_page* page = reinterpret_cast<slab_page*>(
                blk->payload + (i - 1) * PAGE_16K);
            page->next = head;
            head = page;
        }
        free_16k_ = head;
        return true;
    }

    size_t count_free_4k_locked() const noexcept {
        size_t n = 0;
        for (const slab_page* p = free_4k_; p != nullptr; p = p->next) {
            ++n;
        }
        return n;
    }

    size_t count_free_16k_locked() const noexcept {
        size_t n = 0;
        for (const slab_page* p = free_16k_; p != nullptr; p = p->next) {
            ++n;
        }
        return n;
    }
};

inline systems_slab& global_systems_slab() noexcept {
    static systems_slab slab;
    return slab;
}

inline void systems_slab_init() noexcept {
    global_systems_slab().init();
}

inline void systems_slab_shutdown() noexcept {
    global_systems_slab().shutdown();
}

} // namespace df