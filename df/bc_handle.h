#pragma once

// bc_handle.h
//
// Generational handle: (index, generation) packed into uint64_t. Used with
// df::slot_map<T> to give out stable references that become detectably
// invalid when the underlying slot is reused.
//
// handle<T> is a strong typedef: handle<Entity> and handle<Texture> are
// distinct types even though both are 8 bytes. sizeof(handle<T>) == 8 for
// any T.
//
// Encoding: low 32 bits = index, high 32 bits = generation. A freshly
// created handle has generation 1 (0 is reserved for "null handle").
//
// Generation wrap: after 2^32 reuse cycles of the same slot index, the
// generation wraps. Effectively infinite for a game engine.
//
// Style: snake_case, One True Brace Style, template param T.

#include <stddef.h>
#include <stdint.h>

namespace df {

template <typename T>
class handle {
public:
    static constexpr uint32_t null_index = 0xFFFFFFFFu;
    static constexpr uint32_t first_generation = 1u;

    uint32_t index      = null_index;
    uint32_t generation = 0u;

    constexpr handle() noexcept = default;

    constexpr handle(uint32_t idx, uint32_t gen) noexcept
        : index(idx), generation(gen) {}

    uint64_t to_u64() const noexcept {
        return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index);
    }

    static handle from_u64(uint64_t v) noexcept {
        handle h;
        h.index = static_cast<uint32_t>(v & 0xFFFFFFFFu);
        h.generation = static_cast<uint32_t>(v >> 32);
        return h;
    }

    constexpr bool is_null() const noexcept {
        return generation == 0u || index == null_index;
    }

    constexpr explicit operator bool() const noexcept {
        return !is_null();
    }

    friend constexpr bool operator==(handle a, handle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(handle a, handle b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(handle a, handle b) noexcept {
        return a.to_u64() < b.to_u64();
    }
    friend constexpr bool operator>(handle a, handle b) noexcept {
        return a.to_u64() > b.to_u64();
    }
    friend constexpr bool operator<=(handle a, handle b) noexcept {
        return a.to_u64() <= b.to_u64();
    }
    friend constexpr bool operator>=(handle a, handle b) noexcept {
        return a.to_u64() >= b.to_u64();
    }
};

template <typename T>
inline uint64_t hash_handle(handle<T> h) noexcept {
    return h.to_u64();
}

} // namespace df