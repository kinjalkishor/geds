# geds
game engine data structures  
This library uses AI generated code.  

This library contains High Performance allocators, containers and ECS (EnTT like) for use with a Game Engine.  
These allocators and containers along with D3D12MA (D3D12 Memory Allocator) and VMA (Vulkan Memory Allocator) are sufficient even for AAA modern games.  

All our bc_ containers use our own hybrid_pool_allocator backed by systems_slab allocator backed by malloc to minimize malloc calls to minimum. Single allocator is necessary to have move working on containers for maximum performance. If you use std::alllocator interface then 2 containers using 2 different allocators (any functions taking containers will need to template on allocator type also) are 2 different types and if using std::pmr allocators container types are same but allocator types will not match thus disabling moves between them.  
Our all containers use ptrdiff_t platform defined maximum signed type for size internally, use size() for unsigned count and ssize() for signed count just like std containers.  
Another big benefit of our containers is very fast compile times which is a big issue with std containers. And if our containers are used it will be binary compatible on all compilers unlike std containers. For use with 3rd party libraries one will need to take data from their containers to our containers just like one does if they use std containers and other library uses different containers.  

Our open addressing hash map containers are based on excellent martinus/unordered_dense and wangyi-fudan/wyhash.
We use our own containers because node based std containers do not expose node size in portable way so we can not use our allocators as std allocator or std::pmr allocator (which also have the same limitation) with std containers.  

If someone wants to use std containers only for compatibility with their code base we provide bcs_ open addressing hash map, set, ordered map, ordered set, segmented deque which are missing in C++ std library.

Details of library are below :-  

# df — Containers, Allocators & ECS

A header-only C++17 data-structure library for the df game engine. Every container routes through a single allocator abstraction, so the same code runs pool-backed inside the engine and malloc-backed anywhere else.

---

## Allocators

Three tiers, all reached through `df::allocate` / `df::deallocate` / `df::reallocate`:

| Component | Header | Role |
|---|---|---|
| `hybrid_pool_allocator` | `bc_allocator.h` | 11 size classes (32 B – 2 KB), per-thread via `set_active_pool`. Draws chunks from the systems slab with a malloc fallback. |
| `systems_slab` | `bc_systems_slab.h` | Two-tier page allocator (4 KiB / 16 KiB), 4 MiB blocks, spinlock-protected freelists. Backs pool chunk growth. |
| `aligned_alloc_raw` / `alloc_raw` | `bc_raw_alloc.h` | 64-byte-aligned raw allocation. The fallback when no pool is registered. |

Plus non-container allocators:

- **`linear_arena`** / **`frame_arena`** — bump-pointer scratch, per-thread frame reset with RAII scopes.
- **`virtual_arena`** — reserve-then-commit over `VirtualAlloc` / `mmap`, for large streamed assets.
- **`free_list_allocator`** — variable-size, random-lifetime, 32 B – 64 KiB classes plus bypass.
- **`object_pool<T>`** — typed fixed-size object pool.

### How pool-backed containers work

```
container → df::allocate → hybrid_pool_allocator → systems_slab → malloc
                         ↘ aligned_alloc_raw (no pool on this thread)
```

Containers never touch the pool or slab directly. They call `df::allocate`, which dispatches to the thread's active pool if one is registered, else to `aligned_alloc_raw`. Register once per thread:

```cpp
df::hybrid_pool_allocator pool;
df::set_active_pool(&pool);
// ... all df:: containers now use `pool` ...
df::set_active_pool(nullptr);
```

The **`sdf::` namespace** (`bcs_*.h`) provides the same containers with `aligned_alloc_raw` wired in directly — no pool, no `set_active_pool`, usable from any thread or context. Same API, same semantics.

---

## Containers

All containers use `ptrdiff_t` internally, expose `ssize()` / `scapacity()` alongside `size()` / `capacity()`, and come in `df::` (pool) and `sdf::` (malloc) variants.

### Sequences

| Container | Header | Notes |
|---|---|---|
| `vector<T>` | `bc_vector.h` | Contiguous, `memcpy` fast paths for trivial types |
| `deque<T>` | `bc_deque.h` / `bcs_deque.h` | Segmented, random-access iterators; `sdf::` has full `insert`/`erase` |
| `ring_deque<T>` | `bc_ring_deque.h` | Fixed-capacity ring, no growth |
| `ring_buffer<T>` | `bc_ring_buffer.h` | Overwriting history buffer |
| `atomic_ring_buffer<T>` | `bc_atomic_ring_buffer.h` | Lock-free SPSC |
| `stack_vector<T, N>` | `bc_stack_vector.h` | Inline fixed-capacity |
| `list<T>` | `bc_list.h` | Doubly linked, `sort` / `merge` / `splice` |
| `priority_queue<T>` | `bc_priority_queue.h` | Binary heap over `vector<T>` |

### Maps & sets

| Container | Header | Iteration order |
|---|---|---|
| `map<K, V>` / `set<K>` | `bc_map.h` / `bc_set.h` | Sorted (red-black tree, `pair<const K, V>`) |
| `unordered_map<K, V>` / `unordered_set<K>` | `bc_unordered_map.h` / `bc_unordered_set.h` | Chained, insertion-stable nodes |
| `unord_dense_map<K, V>` / `unord_dense_set<K>` | `bc_unord_dense_map.h` / `bc_unord_dense_set.h` | Open addressing, dense values (ankerl-style) |
| `ord_dense_map<K, V>` / `ord_dense_set<K>` | `bc_ord_dense_map.h` / `bc_ord_dense_set.h` | Insertion-ordered dense |
| `lru_cache<K, V>` | `bc_lru_cache.h` | Fixed capacity, O(1) get/put |
| `sparse_set<T, Entity>` | `bc_sparse_set.h` | The ECS component store |
| `slot_map<T, Tag>` | `bc_slot_map.h` | Generational handles |

### Strings & bits

| Container | Header |
|---|---|
| `basic_string<CharT>` / `string` | `bc_string.h` |
| `basic_string_view<CharT>` | `bc_string_view.h` |
| `utf8_string` + `bc_text_codec.h` | UTF-8 / 16 / 32 conversion |
| `basic_string_builder<CharT>` | `bc_string_builder.h` |
| `bitset<N>` / `bit_array` | `bc_bitset.h` / `bc_bit_array.h` |
| `stack_basic_string<T, N>` | `bc_stack_string.h` |

### Views & helpers

`span<T>` (`bc_span.h`), sorted-vector ops (`bc_sorted_vector_ops.h`), trim/split/join (`bc_string_ops.h`).

---

## ECS

`df::ecs::world` is a small, EnTT-style entity-component-system:

- **Generational handles** — `entity = handle<uint32_t>`; stale handles are detected exactly, never dangling.
- **Sparse-set component storage** — O(1) insert / erase / lookup, cache-friendly iteration, swap-and-pop removal.
- **No registration, macros, or RTTI** — components are plain structs, systems are caller-run lambdas.
- **`for_each`** with up to 4 component types via explicit overloads (no variadic tuple machinery).
- **Pool-backed** — component storages route through `df::allocate`.

```cpp
struct position { float x, y; };
struct velocity { float dx, dy; };

df::ecs::world w;
df::ecs::entity e = w.create_entity();
w.add<position>(e, { 10.0f, 20.0f });
w.add<velocity>(e, { 1.0f, 0.0f });

w.for_each<position, velocity>([dt](df::ecs::entity e, position& p, velocity& v) {
    p.x += v.dx * dt;
    p.y += v.dy * dt;
});
```

Destroying an entity removes it from every storage; `get<T>(e)` returns `nullptr` for a stale handle.

---

## Conventions

- `snake_case`, One True Brace Style, template params `T` / `K` / `V` / `CharT`.
- `ptrdiff_t` internal sizes; `ssize()` signed, `size()` unsigned.
- Assertions in debug, UB in release (no exceptions on container hot paths).
- `df::` = pool-backed, `sdf::` = malloc-backed. Identical API.
- `at()` asserts rather than throws (engine-style; no `std::out_of_range`).
- `basic_string::npos` is signed `ptrdiff_t(-1)` — internally consistent, but not `std::string::npos`-compatible.

Header-only. No dependencies beyond the C/C++ standard library.

---
## Thread Safety

The library is built for **single-threaded container use with per-thread allocator ownership**, with one deliberate exception for cross-thread pipelines.

| Component | Thread safe | Notes |
|---|---|---|
| `atomic_ring_buffer<T>` | ✅ SPSC | Lock-free. Exactly one producer, exactly one consumer. |
| `systems_slab` | ✅ Yes | Spinlock-protected freelists. |
| `aligned_alloc_raw` / `alloc_raw` | ✅ Yes | `posix_memalign` / `_aligned_malloc`. |
| `hybrid_pool_allocator` | ⚠️ Per-thread | One pool per thread. Sharing one pool across threads is UB. |
| `set_active_pool` / `get_active_pool` | ✅ Yes | `thread_local`. |
| Every other container | ❌ No | No synchronization. Same model as `std::`. |
| `ecs::world` | ❌ No | Single-threaded. |

**Allocator lifetime rule:** `df::` containers route through the thread-local active pool. A container must be created, used, and destroyed on a thread with the same (or no) pool registered. `sdf::` containers always use `aligned_alloc_raw` and have no such constraint — safe to create on one thread and destroy on another (still not safe for concurrent mutation).

---
## Performance vs. STL / EASTL / ETL

No formal benchmarks yet — this is a design-level comparison, not measured numbers. Treat it as a roadmap, not a claim.

| Aspect | `df::` | `std::` | EASTL | ETL |
|---|---|---|---|---|
| **Allocation** | Pool-backed, per-thread, size-classed | General-purpose `malloc` | Allocator-parameterized, pool-friendly | No dynamic allocation (fixed-capacity) |
| **Hot-path allocations** | O(1), no malloc after warm-up | O(1) but `malloc` per call | O(1) | Zero |
| **Iterator debug overhead** | None | Implementation-defined | Heavy (`EA_DEBUG`) | None |
| **Exception use** | None on container paths | Throws on `at`, `reserve` | Configurable | None |
| **Type size** | Minimal (`ptr`, `len`, `cap`) | Implementation-defined | Larger in debug | Fixed inline |
| **Compile time** | Low (single-header, no traits metaprogramming) | High | High | Moderate |

**Expected characteristics:**

- **vs. `std::`** — `df::` containers should match or beat `std::vector` / `std::deque` for allocation-heavy workloads because the pool eliminates per-node `malloc` calls. `unord_dense_map` (ankerl-style) is likely faster than `std::unordered_map` for lookup-heavy code. `map` and `unordered_map` are structurally similar to `std::` and should be roughly comparable.
- **vs. EASTL** — EASTL is more mature and has deeper optimizer hooks; expect `df::` to be competitive on allocation but not necessarily faster on raw iteration. EASTL's debug builds are much heavier.
- **vs. ETL** — ETL has **zero heap allocation** by design and wins outright on determinism and embedded use. `df::` trades that for unbounded growth.

**The real win** is allocator integration: the same container code runs pool-backed inside the engine and malloc-backed elsewhere (`sdf::`), with no API change. EASTL requires allocator plumbing per container; ETL forbids growth entirely.

Benchmarks against STL/EASTL/ETL are a **TODO** — contributions welcome.
