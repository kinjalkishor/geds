#pragma once

// bc_map.h
//
// Ordered tree map: red-black tree, node-based, pool-backed.
//
// API mirrors std::map:
//   - insert_or_assign, try_insert, operator[], at, find, contains, erase
//   - lower_bound, upper_bound, equal_range, min, max
//   - iterators (bidirectional), for_each
//   - size, ssize, empty, clear, count
//
// Notable properties:
//   - value_type is pair<const K, V>: the key is immutable through iterators,
//     so mutating a key can't silently break the tree's ordering invariant.
//     Matches std::map. The iterator returns pair<const K, V>& (map) or
//     const K& (set).
//   - Erase transplants the successor NODE, not its value: pointer moves only,
//     never value copies. This is what allows pair<const K, V> to work and
//     preserves the invariant that erase invalidates only iterators to the
//     erased element (not to the successor).
//   - Nodes are one df::allocate(sizeof(node)) each -- same as list<T> and
//     unordered_map. The hybrid pool buckets a node into the matching size
//     class, so the allocator story is unified across all containers.
//   - No Compare template parameter: operator< on K is used.
//   - V == void specialization: iterator yields const K&,
//     at / operator[] / insert_or_assign static_assert-fail. df::set<K>
//     on top exposes the set-shaped API.
//   - count() returns size_t, matching std::map::count.
//
// Pool-backed via df::allocate / df::deallocate.
//
// Style: snake_case, One True Brace Style, template params K, V, Fn.

#include "bc_allocator.h"
#include "bc_hash.h"
#include "bc_pair.h"

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

namespace detail {

template <typename K, typename V>
struct map_node_traits {
    using value_type = pair<const K, V>;

    template <typename KK, typename VV>
    static void construct(value_type* at, KK&& k, VV&& v) {
        new (&at->first)  const K(std::forward<KK>(k));
        new (&at->second) V(std::forward<VV>(v));
    }
    static void construct_default(value_type* at, const K& k) {
        new (&at->first)  const K(k);
        new (&at->second) V();
    }
    static void destroy(value_type* at) noexcept {
        at->second.~V();
        at->first.~K();
    }
    static const K& key_of(const value_type& v) noexcept { return v.first; }
};

template <typename K>
struct map_node_traits<K, void> {
    using value_type = K;

    static void construct(value_type* at, const K& k) {
        new (at) K(k);
    }
    static void construct(value_type* at, K&& k) {
        new (at) K(std::move(k));
    }
    static void destroy(value_type* at) noexcept {
        at->~K();
    }
    static const K& key_of(const value_type& v) noexcept { return v; }
};

} // namespace detail

template <typename K, typename V>
class map {
public:
    using key_type    = K;
    using mapped_type = V;
    using value_type  = typename detail::map_node_traits<K, V>::value_type;
    using size_type   = size_t;

    static constexpr bool is_set_v = std::is_void_v<V>;

private:
    using traits = detail::map_node_traits<K, V>;

    struct node {
        node* parent;
        node* left;
        node* right;
        bool  red;
        value_type value;
    };

    node*     root_ = nullptr;
    ptrdiff_t len_  = 0;

public:
    template <bool IsConst>
    class iter_t {
        using node_ptr = std::conditional_t<IsConst, const node*, node*>;
        using value_ref = std::conditional_t<
            IsConst || is_set_v,
            const value_type&,
            value_type&>;
        using value_pp = std::conditional_t<
            IsConst || is_set_v,
            const value_type*,
            value_type*>;

        node_ptr cur_ = nullptr;

    public:
        using value_type        = map::value_type;
        using difference_type   = ptrdiff_t;
        using reference         = value_ref;
        using pointer           = value_pp;
        using iterator_category = std::bidirectional_iterator_tag;

        iter_t() noexcept = default;
        explicit iter_t(node_ptr n) noexcept : cur_(n) {}

        template <bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
        iter_t(iter_t<OtherConst> const& other) noexcept : cur_(other.cur_) {}

        reference operator*()  const noexcept { return cur_->value; }
        pointer   operator->() const noexcept { return &cur_->value; }

        iter_t& operator++() noexcept {
            cur_ = successor(cur_);
            return *this;
        }
        iter_t operator++(int) noexcept {
            iter_t t(*this);
            ++(*this);
            return t;
        }
        iter_t& operator--() noexcept {
            cur_ = predecessor(cur_);
            return *this;
        }
        iter_t operator--(int) noexcept {
            iter_t t(*this);
            --(*this);
            return t;
        }

        template <bool OtherConst>
        bool operator==(iter_t<OtherConst> const& o) const noexcept { return cur_ == o.cur_; }
        template <bool OtherConst>
        bool operator!=(iter_t<OtherConst> const& o) const noexcept { return cur_ != o.cur_; }

        node_ptr raw() const noexcept { return cur_; }

    private:
        static node_ptr minimum(node_ptr n) noexcept {
            while (n != nullptr && n->left != nullptr) {
                n = n->left;
            }
            return n;
        }
        static node_ptr maximum(node_ptr n) noexcept {
            while (n != nullptr && n->right != nullptr) {
                n = n->right;
            }
            return n;
        }
        static node_ptr successor(node_ptr n) noexcept {
            if (n == nullptr) {
                return nullptr;
            }
            if (n->right != nullptr) {
                return minimum(n->right);
            }
            node_ptr p = n->parent;
            while (p != nullptr && n == p->right) {
                n = p;
                p = p->parent;
            }
            return p;
        }
        static node_ptr predecessor(node_ptr n) noexcept {
            if (n == nullptr) {
                return nullptr;
            }
            if (n->left != nullptr) {
                return maximum(n->left);
            }
            node_ptr p = n->parent;
            while (p != nullptr && n == p->left) {
                n = p;
                p = p->parent;
            }
            return p;
        }
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    map() noexcept {}

    map(const map&) = delete;
    map& operator=(const map&) = delete;

    map(map&& other) noexcept
        : root_(other.root_), len_(other.len_) {
        other.root_ = nullptr;
        other.len_ = 0;
    }

    map& operator=(map&& other) noexcept {
        if (this != &other) {
            destroy_all();
            root_ = other.root_;
            len_ = other.len_;
            other.root_ = nullptr;
            other.len_ = 0;
        }
        return *this;
    }

    ~map() {
        destroy_all();
    }

    size_t    size()  const noexcept { return static_cast<size_t>(len_); }
    ptrdiff_t ssize() const noexcept { return len_; }
    bool      empty() const noexcept { return len_ == 0; }

    iterator begin() noexcept { return iterator(minimum(root_)); }
    iterator end() noexcept { return iterator(nullptr); }
    const_iterator begin() const noexcept { return const_iterator(minimum(root_)); }
    const_iterator end()   const noexcept { return const_iterator(nullptr); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (node* n = minimum(root_); n != nullptr; n = successor(n)) {
            if constexpr (is_set_v) {
                fn(traits::key_of(n->value));
            } else {
                fn(traits::key_of(n->value), n->value.second);
            }
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const node* n = minimum(root_); n != nullptr; n = successor(n)) {
            if constexpr (is_set_v) {
                fn(traits::key_of(n->value));
            } else {
                fn(traits::key_of(n->value), n->value.second);
            }
        }
    }

    void insert_or_assign(const K& key, const V& v) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(key, v);
    }
    void insert_or_assign(K&& key, V&& v) {
        static_assert(!is_set_v, "insert_or_assign is not available on a set");
        do_insert_or_assign(std::move(key), std::move(v));
    }

    bool insert(const K& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        return insert_node_key(key) != nullptr;
    }
    bool insert(K&& key) {
        static_assert(is_set_v, "insert(key) requires V == void; use insert_or_assign(key, value)");
        return insert_node_key(std::move(key)) != nullptr;
    }

    bool try_insert(const K& key, const V& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        return insert_node_kv(key, v) != nullptr;
    }
    bool try_insert(K&& key, V&& v) {
        static_assert(!is_set_v, "try_insert(key, value) requires V != void");
        return insert_node_kv(std::move(key), std::move(v)) != nullptr;
    }

    V& operator[](const K& key) {
        static_assert(!is_set_v, "operator[] is not available on a set");
        node* found = find_node(key);
        if (found != nullptr) {
            return found->value.second;
        }
        node* fresh = insert_node_default(key);
        return fresh->value.second;
    }

    V& at(const K& key) {
        static_assert(!is_set_v, "at() is not available on a set");
        node* n = find_node(key);
        assert(n != nullptr && "map::at key not found");
        return n->value.second;
    }
    const V& at(const K& key) const {
        static_assert(!is_set_v, "at() is not available on a set");
        node* n = find_node(key);
        assert(n != nullptr && "map::at key not found");
        return n->value.second;
    }

    bool erase(const K& key) {
        node* n = find_node(key);
        if (n == nullptr) {
            return false;
        }
        delete_node(n);
        return true;
    }

    iterator erase(const_iterator pos) {
        node* n = const_cast<node*>(pos.raw());
        assert(n != nullptr && "map::erase on end()");
        node* next = successor(n);
        delete_node(n);
        return iterator(next);
    }

    void clear() noexcept {
        destroy_subtree(root_);
        root_ = nullptr;
        len_ = 0;
    }

    void swap(map& other) noexcept {
        std::swap(root_, other.root_);
        std::swap(len_, other.len_);
    }

    value_type* find(const K& key) noexcept {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }
    const value_type* find(const K& key) const noexcept {
        node* n = find_node(key);
        return n != nullptr ? &n->value : nullptr;
    }

    bool contains(const K& key) const noexcept {
        return find_node(key) != nullptr;
    }

    size_t count(const K& key) const noexcept {
        return find_node(key) != nullptr ? 1 : 0;
    }

    iterator lower_bound(const K& key) noexcept {
        return iterator(lower_bound_node(key));
    }
    const_iterator lower_bound(const K& key) const noexcept {
        return const_iterator(lower_bound_node(key));
    }

    iterator upper_bound(const K& key) noexcept {
        return iterator(upper_bound_node(key));
    }
    const_iterator upper_bound(const K& key) const noexcept {
        return const_iterator(upper_bound_node(key));
    }

    pair<iterator, iterator> equal_range(const K& key) noexcept {
        return { lower_bound(key), upper_bound(key) };
    }
    pair<const_iterator, const_iterator> equal_range(const K& key) const noexcept {
        return { lower_bound(key), upper_bound(key) };
    }

    iterator min() noexcept { return iterator(minimum(root_)); }
    const_iterator min() const noexcept { return const_iterator(minimum(root_)); }
    iterator max() noexcept { return iterator(maximum(root_)); }
    const_iterator max() const noexcept { return const_iterator(maximum(root_)); }

private:
    static node* minimum(node* n) noexcept {
        while (n != nullptr && n->left != nullptr) {
            n = n->left;
        }
        return n;
    }
    static const node* minimum(const node* n) noexcept {
        while (n != nullptr && n->left != nullptr) {
            n = n->left;
        }
        return n;
    }
    static node* maximum(node* n) noexcept {
        while (n != nullptr && n->right != nullptr) {
            n = n->right;
        }
        return n;
    }
    static const node* maximum(const node* n) noexcept {
        while (n != nullptr && n->right != nullptr) {
            n = n->right;
        }
        return n;
    }
    static node* successor(node* n) noexcept {
        if (n == nullptr) {
            return nullptr;
        }
        if (n->right != nullptr) {
            return minimum(n->right);
        }
        node* p = n->parent;
        while (p != nullptr && n == p->right) {
            n = p;
            p = p->parent;
        }
        return p;
    }
    static const node* successor(const node* n) noexcept {
        if (n == nullptr) {
            return nullptr;
        }
        if (n->right != nullptr) {
            return minimum(n->right);
        }
        const node* p = n->parent;
        while (p != nullptr && n == p->right) {
            n = p;
            p = p->parent;
        }
        return p;
    }

    static node* alloc_node() {
        void* raw = df::allocate(sizeof(node));
        assert(raw != nullptr && "map: node allocation failed");
        return static_cast<node*>(raw);
    }

    static void free_node(node* n) {
        df::deallocate(n, sizeof(node));
    }

    template <typename KK, typename VV>
    node* make_node_kv(KK&& key, VV&& v) {
        node* n = alloc_node();
        traits::construct(&n->value, std::forward<KK>(key), std::forward<VV>(v));
        n->parent = nullptr;
        n->left = nullptr;
        n->right = nullptr;
        n->red = true;
        return n;
    }

    template <typename KK>
    node* make_node_key(KK&& key) {
        node* n = alloc_node();
        traits::construct(&n->value, std::forward<KK>(key));
        n->parent = nullptr;
        n->left = nullptr;
        n->right = nullptr;
        n->red = true;
        return n;
    }

    node* make_node_default(const K& key) {
        node* n = alloc_node();
        traits::construct_default(&n->value, key);
        n->parent = nullptr;
        n->left = nullptr;
        n->right = nullptr;
        n->red = true;
        return n;
    }

    static void destroy_node(node* n) noexcept {
        traits::destroy(&n->value);
        free_node(n);
    }

    void destroy_subtree(node* n) noexcept {
        node* cur = n;
        node* prev = nullptr;
        while (cur != nullptr) {
            if (prev == cur->parent) {
                if (cur->left != nullptr) {
                    prev = cur;
                    cur = cur->left;
                } else if (cur->right != nullptr) {
                    prev = cur;
                    cur = cur->right;
                } else {
                    node* parent = cur->parent;
                    node* to_free = cur;
                    prev = cur;
                    cur = parent;
                    destroy_node(to_free);
                }
            } else if (prev == cur->left) {
                if (cur->right != nullptr) {
                    prev = cur;
                    cur = cur->right;
                } else {
                    node* parent = cur->parent;
                    node* to_free = cur;
                    prev = cur;
                    cur = parent;
                    destroy_node(to_free);
                }
            } else {
                node* parent = cur->parent;
                node* to_free = cur;
                prev = cur;
                cur = parent;
                destroy_node(to_free);
            }
        }
    }

    void destroy_all() noexcept {
        destroy_subtree(root_);
        root_ = nullptr;
        len_ = 0;
    }

    static bool key_less(const K& a, const K& b) noexcept(noexcept(a < b)) {
        return a < b;
    }

    node* find_node(const K& key) const noexcept {
        node* n = root_;
        while (n != nullptr) {
            const K& nk = traits::key_of(n->value);
            if (key_less(key, nk)) {
                n = n->left;
            } else if (key_less(nk, key)) {
                n = n->right;
            } else {
                return n;
            }
        }
        return nullptr;
    }

    node* lower_bound_node(const K& key) const noexcept {
        node* n = root_;
        node* result = nullptr;
        while (n != nullptr) {
            const K& nk = traits::key_of(n->value);
            if (!key_less(nk, key)) {
                result = n;
                n = n->left;
            } else {
                n = n->right;
            }
        }
        return result;
    }

    node* upper_bound_node(const K& key) const noexcept {
        node* n = root_;
        node* result = nullptr;
        while (n != nullptr) {
            const K& nk = traits::key_of(n->value);
            if (key_less(key, nk)) {
                result = n;
                n = n->left;
            } else {
                n = n->right;
            }
        }
        return result;
    }

    struct insert_location {
        node* parent;
        bool  inserted_left;
        node* existing;
    };

    insert_location locate(const K& key) const noexcept {
        node* parent = nullptr;
        node* cur = root_;
        bool went_left = false;
        while (cur != nullptr) {
            parent = cur;
            const K& nk = traits::key_of(cur->value);
            if (key_less(key, nk)) {
                cur = cur->left;
                went_left = true;
            } else if (key_less(nk, key)) {
                cur = cur->right;
                went_left = false;
            } else {
                return { nullptr, false, cur };
            }
        }
        return { parent, went_left, nullptr };
    }

    void link_and_rebalance(node* fresh, node* parent, bool inserted_left) {
        fresh->parent = parent;
        fresh->red = true;
        if (parent == nullptr) {
            root_ = fresh;
        } else if (inserted_left) {
            parent->left = fresh;
        } else {
            parent->right = fresh;
        }
        insert_fixup(fresh);
        ++len_;
    }

    template <typename KK, typename VV>
    node* insert_node_kv(KK&& key, VV&& v) {
        insert_location loc = locate(key);
        if (loc.existing != nullptr) {
            return nullptr;
        }
        node* fresh = make_node_kv(std::forward<KK>(key), std::forward<VV>(v));
        link_and_rebalance(fresh, loc.parent, loc.inserted_left);
        return fresh;
    }

    template <typename KK>
    node* insert_node_key(KK&& key) {
        insert_location loc = locate(key);
        if (loc.existing != nullptr) {
            return nullptr;
        }
        node* fresh = make_node_key(std::forward<KK>(key));
        link_and_rebalance(fresh, loc.parent, loc.inserted_left);
        return fresh;
    }

    node* insert_node_default(const K& key) {
        insert_location loc = locate(key);
        if (loc.existing != nullptr) {
            return loc.existing;
        }
        node* fresh = make_node_default(key);
        link_and_rebalance(fresh, loc.parent, loc.inserted_left);
        return fresh;
    }

    template <typename KK, typename VV>
    void do_insert_or_assign(KK&& key, VV&& v) {
        insert_location loc = locate(key);
        if (loc.existing != nullptr) {
            loc.existing->value.second = std::forward<VV>(v);
            return;
        }
        node* fresh = make_node_kv(std::forward<KK>(key), std::forward<VV>(v));
        link_and_rebalance(fresh, loc.parent, loc.inserted_left);
    }

    void delete_node(node* z) noexcept {
        node* y = z;
        node* x = nullptr;
        node* x_parent = nullptr;
        bool  x_was_left = false;
        bool  y_was_red = y->red;

        if (z->left == nullptr) {
            x = z->right;
            x_parent = z->parent;
            x_was_left = (z->parent != nullptr && z->parent->left == z);
            transplant(z, z->right);
        } else if (z->right == nullptr) {
            x = z->left;
            x_parent = z->parent;
            x_was_left = (z->parent != nullptr && z->parent->left == z);
            transplant(z, z->left);
        } else {
            y = minimum(z->right);
            y_was_red = y->red;
            x = y->right;
            if (y->parent == z) {
                x_parent = y;
                x_was_left = false;
                if (x != nullptr) {
                    x->parent = y;
                }
            } else {
                x_parent = y->parent;
                x_was_left = (y->parent->left == y);
                transplant(y, y->right);
                y->right = z->right;
                y->right->parent = y;
            }
            transplant(z, y);
            y->left = z->left;
            y->left->parent = y;
            y->red = z->red;
        }

        if (!y_was_red) {
            delete_fixup(x, x_parent, x_was_left);
        }

        destroy_node(z);
        --len_;
    }

    void transplant(node* u, node* v) noexcept {
        if (u->parent == nullptr) {
            root_ = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        if (v != nullptr) {
            v->parent = u->parent;
        }
    }

    void rotate_left(node* x) noexcept {
        node* y = x->right;
        x->right = y->left;
        if (y->left != nullptr) {
            y->left->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root_ = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->left = x;
        x->parent = y;
    }

    void rotate_right(node* x) noexcept {
        node* y = x->left;
        x->left = y->right;
        if (y->right != nullptr) {
            y->right->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root_ = y;
        } else if (x == x->parent->right) {
            x->parent->right = y;
        } else {
            x->parent->left = y;
        }
        y->right = x;
        x->parent = y;
    }

    void insert_fixup(node* z) noexcept {
        while (z->parent != nullptr && z->parent->red) {
            node* gp = z->parent->parent;
            if (z->parent == gp->left) {
                node* uncle = gp->right;
                if (uncle != nullptr && uncle->red) {
                    z->parent->red = false;
                    uncle->red = false;
                    gp->red = true;
                    z = gp;
                } else {
                    if (z == z->parent->right) {
                        z = z->parent;
                        rotate_left(z);
                    }
                    z->parent->red = false;
                    gp->red = true;
                    rotate_right(gp);
                }
            } else {
                node* uncle = gp->left;
                if (uncle != nullptr && uncle->red) {
                    z->parent->red = false;
                    uncle->red = false;
                    gp->red = true;
                    z = gp;
                } else {
                    if (z == z->parent->left) {
                        z = z->parent;
                        rotate_right(z);
                    }
                    z->parent->red = false;
                    gp->red = true;
                    rotate_left(gp);
                }
            }
        }
        root_->red = false;
    }

    void delete_fixup(node* x, node* parent, bool was_left) noexcept {
        while (x != root_ && (x == nullptr || !x->red)) {
            if (parent == nullptr) {
                break;
            }
            if (was_left) {
                node* sibling = parent->right;
                if (sibling != nullptr && sibling->red) {
                    sibling->red = false;
                    parent->red = true;
                    rotate_left(parent);
                    sibling = parent->right;
                }
                const bool sibling_left_black =
                    (sibling == nullptr) || (sibling->left == nullptr || !sibling->left->red);
                const bool sibling_right_black =
                    (sibling == nullptr) || (sibling->right == nullptr || !sibling->right->red);
                if (sibling_left_black && sibling_right_black) {
                    if (sibling != nullptr) {
                        sibling->red = true;
                    }
                    x = parent;
                    parent = x->parent;
                    was_left = (parent != nullptr && parent->left == x);
                } else {
                    if (sibling_right_black) {
                        if (sibling->left != nullptr) {
                            sibling->left->red = false;
                        }
                        sibling->red = true;
                        rotate_right(sibling);
                        sibling = parent->right;
                    }
                    if (sibling != nullptr) {
                        sibling->red = parent->red;
                    }
                    parent->red = false;
                    if (sibling != nullptr && sibling->right != nullptr) {
                        sibling->right->red = false;
                    }
                    rotate_left(parent);
                    x = root_;
                    parent = nullptr;
                }
            } else {
                node* sibling = parent->left;
                if (sibling != nullptr && sibling->red) {
                    sibling->red = false;
                    parent->red = true;
                    rotate_right(parent);
                    sibling = parent->left;
                }
                const bool sibling_right_black =
                    (sibling == nullptr) || (sibling->right == nullptr || !sibling->right->red);
                const bool sibling_left_black =
                    (sibling == nullptr) || (sibling->left == nullptr || !sibling->left->red);
                if (sibling_right_black && sibling_left_black) {
                    if (sibling != nullptr) {
                        sibling->red = true;
                    }
                    x = parent;
                    parent = x->parent;
                    was_left = (parent != nullptr && parent->left == x);
                } else {
                    if (sibling_left_black) {
                        if (sibling->right != nullptr) {
                            sibling->right->red = false;
                        }
                        sibling->red = true;
                        rotate_left(sibling);
                        sibling = parent->left;
                    }
                    if (sibling != nullptr) {
                        sibling->red = parent->red;
                    }
                    parent->red = false;
                    if (sibling != nullptr && sibling->left != nullptr) {
                        sibling->left->red = false;
                    }
                    rotate_right(parent);
                    x = root_;
                    parent = nullptr;
                }
            }
        }
        if (x != nullptr) {
            x->red = false;
        }
    }
};

template <typename K, typename V>
inline void swap(map<K, V>& a, map<K, V>& b) noexcept {
    a.swap(b);
}

} // namespace df