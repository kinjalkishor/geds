#pragma once

// bc_ecs.h
//
// Small, Unity-flavoured entity-component-system for the df game engine.
// Single header. No registration, no macros, no reflection, no global
// state. Components are plain structs. Entities are lightweight generational
// handles. Systems are caller-run lambdas.
//
// Usage:
//
//   struct position { float x, y; };
//   struct velocity { float dx, dy; };
//
//   df::ecs::world w;
//
//   df::ecs::entity e = w.create_entity();
//   w.add<position>(e, { 10.0f, 20.0f });
//   w.add<velocity>(e, { 1.0f, 0.0f });
//
//   w.for_each<position, velocity>([dt](df::ecs::entity e, position& p, velocity& v) {
//       p.x += v.dx * dt;
//       p.y += v.dy * dt;
//   });
//
// Destroying an entity invalidates its handle; subsequent get<T>(e) returns
// nullptr, and the entity will not appear in for_each. Stale handles are
// detected exactly (generational handles).
//
// Design choices:
//   - for_each is capped at 4 component types via explicit overloads. No
//     variadic fold, no tuple machinery. Systems needing more should
//     bundle components into one composite type.
//   - Component storage is keyed by the full entity handle (index +
//     generation), not just the slot index. Stale handles return nullptr
//     from get<T>() exactly.
//   - All storages are pool-backed (df::allocate / df::deallocate).
//   - Single header, no .cpp, no macros, no RTTI.
//   - reserve_entity(n) forwards to the internal slot_map reserve.
//   - The entity slot map is instantiated as slot_map<entity_record, uint32_t>,
//     so its handle type is handle<uint32_t> == df::ecs::entity. The stored
//     value type (entity_record) and the handle tag (uint32_t) are independent.
//
// Style: snake_case, One True Brace Style, template params T / Fn.

#include "bc_handle.h"
#include "bc_sparse_set.h"
#include "bc_slot_map.h"
#include "bc_vector.h"
#include "bc_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {
namespace ecs {

using entity = df::handle<uint32_t>;

namespace detail {

inline uint64_t entity_key(entity e) noexcept {
    return e.to_u64();
}

inline entity entity_from_key(uint64_t k) noexcept {
    return entity::from_u64(k);
}

inline uint32_t next_component_index() noexcept {
    static uint32_t next = 0;
    return next++;
}

template <typename T>
uint32_t component_index() noexcept {
    static const uint32_t id = next_component_index();
    return id;
}

class storage_base {
public:
    virtual ~storage_base() = default;
    virtual void remove_entity_if_present(uint64_t key) = 0;
    virtual void clear() = 0;
    virtual void destroy_and_deallocate() = 0;
};

template <typename T>
class storage_for : public storage_base {
public:
    sparse_set<T, uint64_t> data;

    void remove_entity_if_present(uint64_t key) override {
        data.erase(key);
    }

    void clear() override {
        data.clear();
    }

    void destroy_and_deallocate() override {
        this->~storage_for();
        df::deallocate(this, sizeof(storage_for<T>));
    }
};

} // namespace detail

class world {
public:
    world() noexcept = default;

    world(const world&) = delete;
    world& operator=(const world&) = delete;

    world(world&&) noexcept = default;
    world& operator=(world&&) noexcept = default;

    ~world() {
        destroy_storages();
    }

    entity create_entity() {
        return entities_.insert(entity_record{});
    }

    void reserve_entity(ptrdiff_t n) {
        entities_.reserve(n);
    }

    bool destroy_entity(entity e) {
        if (!entities_.contains(e)) {
            return false;
        }
        const uint64_t key = detail::entity_key(e);
        for (ptrdiff_t i = 0; i < storages_.ssize(); ++i) {
            if (storages_[i] != nullptr) {
                storages_[i]->remove_entity_if_present(key);
            }
        }
        return entities_.erase(e);
    }

    bool alive(entity e) const {
        return entities_.contains(e);
    }

    size_t entity_count() const noexcept {
        return entities_.size();
    }

    template <typename T>
    T& add(entity e, const T& value = T{}) {
        assert(alive(e) && "ecs::world::add on dead entity");
        sparse_set<T, uint64_t>& storage = storage_for<T>();
        storage.insert(detail::entity_key(e), value);
        return storage.at(detail::entity_key(e));
    }

    template <typename T, typename... Args>
    T& emplace(entity e, Args&&... args) {
        assert(alive(e) && "ecs::world::emplace on dead entity");
        sparse_set<T, uint64_t>& storage = storage_for<T>();
        storage.emplace(detail::entity_key(e), std::forward<Args>(args)...);
        return storage.at(detail::entity_key(e));
    }

    template <typename T>
    T* get(entity e) {
        if (!alive(e)) {
            return nullptr;
        }
        detail::storage_for<T>* s = storage_for_opt<T>();
        if (s == nullptr) {
            return nullptr;
        }
        return s->data.get(detail::entity_key(e));
    }

    template <typename T>
    const T* get(entity e) const {
        if (!alive(e)) {
            return nullptr;
        }
        const detail::storage_for<T>* s = storage_for_opt<T>();
        if (s == nullptr) {
            return nullptr;
        }
        return s->data.get(detail::entity_key(e));
    }

    template <typename T>
    bool has(entity e) const {
        return get<T>(e) != nullptr;
    }

    template <typename T>
    bool remove(entity e) {
        if (!alive(e)) {
            return false;
        }
        detail::storage_for<T>* s = storage_for_opt<T>();
        if (s == nullptr) {
            return false;
        }
        return s->data.erase(detail::entity_key(e));
    }

    template <typename A, typename Fn>
    void for_each(Fn&& fn) {
        detail::storage_for<A>* sa = storage_for_opt<A>();
        if (sa == nullptr) {
            return;
        }
        sparse_set<A, uint64_t>& s = sa->data;
        const uint64_t* keys = s.entities_begin();
        A* vals = s.data();
        const ptrdiff_t n = s.ssize();
        for (ptrdiff_t i = 0; i < n; ++i) {
            fn(detail::entity_from_key(keys[i]), vals[i]);
        }
    }

    template <typename A, typename B, typename Fn>
    void for_each(Fn&& fn) {
        detail::storage_for<A>* sa = storage_for_opt<A>();
        detail::storage_for<B>* sb = storage_for_opt<B>();
        if (sa == nullptr || sb == nullptr) {
            return;
        }
        sparse_set<A, uint64_t>& s = sa->data;
        const uint64_t* keys = s.entities_begin();
        A* vals = s.data();
        const ptrdiff_t n = s.ssize();
        for (ptrdiff_t i = 0; i < n; ++i) {
            const uint64_t key = keys[i];
            B* b = sb->data.get(key);
            if (b == nullptr) {
                continue;
            }
            fn(detail::entity_from_key(key), vals[i], *b);
        }
    }

    template <typename A, typename B, typename C, typename Fn>
    void for_each(Fn&& fn) {
        detail::storage_for<A>* sa = storage_for_opt<A>();
        detail::storage_for<B>* sb = storage_for_opt<B>();
        detail::storage_for<C>* sc = storage_for_opt<C>();
        if (sa == nullptr || sb == nullptr || sc == nullptr) {
            return;
        }
        sparse_set<A, uint64_t>& s = sa->data;
        const uint64_t* keys = s.entities_begin();
        A* vals = s.data();
        const ptrdiff_t n = s.ssize();
        for (ptrdiff_t i = 0; i < n; ++i) {
            const uint64_t key = keys[i];
            B* b = sb->data.get(key);
            if (b == nullptr) {
                continue;
            }
            C* c = sc->data.get(key);
            if (c == nullptr) {
                continue;
            }
            fn(detail::entity_from_key(key), vals[i], *b, *c);
        }
    }

    template <typename A, typename B, typename C, typename D, typename Fn>
    void for_each(Fn&& fn) {
        detail::storage_for<A>* sa = storage_for_opt<A>();
        detail::storage_for<B>* sb = storage_for_opt<B>();
        detail::storage_for<C>* sc = storage_for_opt<C>();
        detail::storage_for<D>* sd = storage_for_opt<D>();
        if (sa == nullptr || sb == nullptr || sc == nullptr || sd == nullptr) {
            return;
        }
        sparse_set<A, uint64_t>& s = sa->data;
        const uint64_t* keys = s.entities_begin();
        A* vals = s.data();
        const ptrdiff_t n = s.ssize();
        for (ptrdiff_t i = 0; i < n; ++i) {
            const uint64_t key = keys[i];
            B* b = sb->data.get(key);
            if (b == nullptr) {
                continue;
            }
            C* c = sc->data.get(key);
            if (c == nullptr) {
                continue;
            }
            D* d = sd->data.get(key);
            if (d == nullptr) {
                continue;
            }
            fn(detail::entity_from_key(key), vals[i], *b, *c, *d);
        }
    }

    void clear() {
        for (ptrdiff_t i = 0; i < storages_.ssize(); ++i) {
            if (storages_[i] != nullptr) {
                storages_[i]->clear();
            }
        }
        entities_.clear();
    }

private:
    struct entity_record {
        uint32_t reserved = 0;
    };

    template <typename T>
    detail::storage_for<T>* storage_for_opt() {
        const uint32_t idx = detail::component_index<T>();
        if (idx >= static_cast<uint32_t>(storages_.ssize())) {
            return nullptr;
        }
        return static_cast<detail::storage_for<T>*>(storages_[idx]);
    }

    template <typename T>
    const detail::storage_for<T>* storage_for_opt() const {
        const uint32_t idx = detail::component_index<T>();
        if (idx >= static_cast<uint32_t>(storages_.ssize())) {
            return nullptr;
        }
        return static_cast<const detail::storage_for<T>*>(storages_[idx]);
    }

    template <typename T>
    sparse_set<T, uint64_t>& storage_for() {
        const uint32_t idx = detail::component_index<T>();
        if (idx >= static_cast<uint32_t>(storages_.ssize())) {
            const ptrdiff_t old_size = storages_.ssize();
            storages_.resize(static_cast<ptrdiff_t>(idx + 1));
            for (ptrdiff_t i = old_size; i <= static_cast<ptrdiff_t>(idx); ++i) {
                storages_[i] = nullptr;
            }
        }
        if (storages_[idx] == nullptr) {
            void* raw = df::allocate(sizeof(detail::storage_for<T>));
            assert(raw != nullptr && "ecs::world: storage allocation failed");
            storages_[idx] = new (raw) detail::storage_for<T>();
        }
        return static_cast<detail::storage_for<T>*>(storages_[idx])->data;
    }

    void destroy_storages() {
        for (ptrdiff_t i = 0; i < storages_.ssize(); ++i) {
            if (storages_[i] != nullptr) {
                storages_[i]->destroy_and_deallocate();
                storages_[i] = nullptr;
            }
        }
    }

    slot_map<entity_record, uint32_t> entities_;
    df::vector<detail::storage_base*> storages_;
};

} // namespace ecs
} // namespace df