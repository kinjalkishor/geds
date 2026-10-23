#pragma once

// bc_raw_alloc.h
//
// Aligned raw allocation helpers. No dependencies beyond the C standard
// library. Used by bc_systems_slab.h, bc_allocator.h, and anything else that
// needs raw aligned memory without pulling in the pool or arena.
//
// Two families:
//   - aligned_alloc_raw(size, align) / aligned_free_raw(p)
//     Explicit alignment. Use when the caller knows it needs a specific
//     alignment (e.g. alignof(over_aligned_T)).
//   - alloc_raw(size) / free_raw(p)
//     df default alignment (64, matching hybrid_pool_allocator::BLOCK_ALIGN
//     and the cache line size on x86-64 / ARM64). Use for general-purpose
//     raw allocation where the caller doesn't have a specific alignment
//     requirement.
//
// alloc_raw is exactly aligned_alloc_raw(size, default_align). free_raw is
// exactly aligned_free_raw. The two families are interchangeable on the
// free side.
//
// Style: snake_case, One True Brace Style.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_MSC_VER)
#   include <malloc.h>
#endif

namespace df {

inline constexpr size_t default_align = 64;

inline void* aligned_alloc_raw(size_t size, size_t align) noexcept {
    assert(align != 0);
    assert((align & (align - 1)) == 0 && "align must be a power of two");
    if (size == 0) {
        size = 1;
    }
#if defined(_MSC_VER)
    return _aligned_malloc(size, align);
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, size) != 0) {
        return nullptr;
    }
    return p;
#endif
}

inline void aligned_free_raw(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    free(p);
#endif
}

inline void* alloc_raw(size_t size) noexcept {
    return aligned_alloc_raw(size, default_align);
}

inline void free_raw(void* p) noexcept {
    aligned_free_raw(p);
}

inline size_t align_up(size_t v, size_t align) noexcept {
    assert((align & (align - 1)) == 0);
    return (v + (align - 1)) & ~(align - 1);
}

} // namespace df