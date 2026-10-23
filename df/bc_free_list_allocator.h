#pragma once

// bc_free_list_allocator.h
//
// General-purpose free-list allocator. Variable-size allocations with
// random lifetime, backed by df::allocate (pool -> slab -> malloc fallback)
// for class-sized blocks, and by aligned_alloc_raw (malloc directly) for
// blocks larger than MAX_SIZE_CLASS.
//
// Size classes at 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
// 32768, 65536 bytes, plus a bypass path for larger allocations.
//
// Style: snake_case, One True Brace Style.

#include "bc_allocator.h"
#include "bc_raw_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

namespace df {

class free_list_allocator {
public:
    static constexpr size_t MIN_SIZE_CLASS   = 32;
    static constexpr size_t MAX_SIZE_CLASS   = 65536;
    static constexpr size_t NUM_SIZE_CLASSES = 12;
    static constexpr size_t HEADER_SIZE      = 16;
    static constexpr size_t BLOCK_ALIGN      = 64;

    free_list_allocator() noexcept = default;

    free_list_allocator(const free_list_allocator&) = delete;
    free_list_allocator& operator=(const free_list_allocator&) = delete;
    free_list_allocator(free_list_allocator&&) = delete;
    free_list_allocator& operator=(free_list_allocator&&) = delete;

    ~free_list_allocator() {
        for (size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
            free_node* n = heads_[c];
            while (n != nullptr) {
                free_node* next = n->next;
                df::deallocate(n, size_of_class(c));
                n = next;
            }
            heads_[c] = nullptr;
        }
    }

    void* allocate(size_t size) noexcept {
        if (size == 0) {
            size = 1;
        }
        const size_t requested = size + HEADER_SIZE;
        if (requested > MAX_SIZE_CLASS) {
            return allocate_bypass(size);
        }
        const size_t class_idx = class_index_for(requested);
        const size_t block_size = size_of_class(class_idx);

        free_node* n = heads_[class_idx];
        if (n == nullptr) {
            void* raw = df::allocate(block_size);
            if (raw == nullptr) {
                return nullptr;
            }
            n = static_cast<free_node*>(raw);
        } else {
            heads_[class_idx] = n->next;
        }
        uint32_t* header = reinterpret_cast<uint32_t*>(n);
        header[0] = static_cast<uint32_t>(class_idx);
        header[1] = 0;
        return reinterpret_cast<unsigned char*>(n) + HEADER_SIZE;
    }

    void deallocate(void* p, size_t size) noexcept {
        if (p == nullptr) {
            return;
        }
        unsigned char* raw = static_cast<unsigned char*>(p) - HEADER_SIZE;
        uint32_t* header = reinterpret_cast<uint32_t*>(raw);
        const uint32_t class_idx = header[0];
        if (class_idx == UINT32_MAX) {
            deallocate_bypass(raw);
            return;
        }
        assert(class_idx < NUM_SIZE_CLASSES);
        (void)size;
        free_node* n = reinterpret_cast<free_node*>(raw);
        n->next = heads_[class_idx];
        heads_[class_idx] = n;
    }

    size_t bytes_free() const noexcept {
        size_t total = 0;
        for (size_t c = 0; c < NUM_SIZE_CLASSES; ++c) {
            for (const free_node* n = heads_[c]; n != nullptr; n = n->next) {
                total += size_of_class(c);
            }
        }
        return total;
    }

private:
    struct free_node {
        free_node* next;
    };

    free_node* heads_[NUM_SIZE_CLASSES] = {};

    static constexpr size_t size_of_class(size_t c) noexcept {
        return MIN_SIZE_CLASS << c;
    }

    static constexpr size_t class_index_for(size_t requested) noexcept {
        size_t idx = 0;
        size_t size = MIN_SIZE_CLASS;
        while (size < requested) {
            size <<= 1;
            ++idx;
        }
        return idx;
    }

    void* allocate_bypass(size_t size) noexcept {
        void* raw = aligned_alloc_raw(size + HEADER_SIZE, BLOCK_ALIGN);
        if (raw == nullptr) {
            return nullptr;
        }
        uint32_t* header = reinterpret_cast<uint32_t*>(raw);
        header[0] = UINT32_MAX;
        header[1] = 0;
        return reinterpret_cast<unsigned char*>(raw) + HEADER_SIZE;
    }

    void deallocate_bypass(void* raw) noexcept {
        aligned_free_raw(raw);
    }
};

} // namespace df