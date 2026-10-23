#pragma once

// bc_frame_arena.h
//
// Per-thread frame arena: one linear_arena per thread, reset at the start
// of each frame, with an RAII frame_scope for function-local scratch.
//
// Model:
//   - Each thread has its own linear_arena (thread_local), lazily
//     initialized with a default capacity (8 MiB) or via
//     frame_arena_init(capacity).
//   - frame_arena_reset() resets the current thread's arena to 0. Call at
//     the start of each frame so the previous frame's data is still
//     readable until then.
//   - frame_allocate(n, align) bumps the current thread's arena.
//   - frame_scope is an RAII object: checkpoints at construction, rewinds
//     to the checkpoint at destruction. Nesting works.
//
// Style: snake_case, One True Brace Style.

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>

namespace df {

namespace detail {

struct frame_arena_tls {
    linear_arena arena;
    bool initialized = false;
};

inline frame_arena_tls& frame_arena_state() noexcept {
    thread_local frame_arena_tls state;
    return state;
}

inline linear_arena& ensure_frame_arena() noexcept {
    frame_arena_tls& state = frame_arena_state();
    if (!state.initialized) {
        const bool ok = state.arena.init(8 * 1024 * 1024, 64);
        assert(ok && "frame_arena: default 8 MiB allocation failed");
        state.initialized = true;
    }
    return state.arena;
}

} // namespace detail

inline void frame_arena_init(size_t capacity_bytes) noexcept {
    detail::frame_arena_tls& state = detail::frame_arena_state();
    if (state.initialized) {
        state.arena.release();
    }
    const bool ok = state.arena.init(capacity_bytes, 64);
    assert(ok && "frame_arena: allocation failed");
    state.initialized = true;
}

inline void frame_arena_reset() noexcept {
    detail::ensure_frame_arena().reset();
}

inline void frame_arena_shutdown() noexcept {
    detail::frame_arena_tls& state = detail::frame_arena_state();
    if (state.initialized) {
        state.arena.release();
        state.initialized = false;
    }
}

inline void* frame_allocate(size_t size, size_t align = 0) noexcept {
    void* p = detail::ensure_frame_arena().try_allocate(size, align);
    assert(p != nullptr && "frame_arena: overflow");
    return p;
}

inline void* frame_try_allocate(size_t size, size_t align = 0) noexcept {
    return detail::ensure_frame_arena().try_allocate(size, align);
}

inline ptrdiff_t frame_arena_checkpoint() noexcept {
    return detail::ensure_frame_arena().get_checkpoint();
}

inline void frame_arena_rewind(ptrdiff_t cp) noexcept {
    detail::ensure_frame_arena().rewind(cp);
}

inline size_t frame_arena_size() noexcept {
    return detail::ensure_frame_arena().size();
}

inline size_t frame_arena_capacity() noexcept {
    return detail::ensure_frame_arena().capacity();
}

class frame_scope {
public:
    frame_scope() noexcept : checkpoint_(frame_arena_checkpoint()) {}

    frame_scope(const frame_scope&) = delete;
    frame_scope& operator=(const frame_scope&) = delete;
    frame_scope(frame_scope&&) = delete;
    frame_scope& operator=(frame_scope&&) = delete;

    ~frame_scope() {
        frame_arena_rewind(checkpoint_);
    }

private:
    ptrdiff_t checkpoint_;
};

} // namespace df