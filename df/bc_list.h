#pragma once

// bc_list.h
//
// Doubly linked list, node-based, pool-backed.
// - templated on T only; no allocator template parameter
// - nodes are df::allocate'd individually; each holds { prev, next, value }
// - bidirectional iterators; stable references/iterators on insert
// - members: head_, tail_, len_
// - ptrdiff_t internal size; size() unsigned, ssize() signed
// - sort / merge / reverse / unique as member functions, matching std::list
// - contains / resize / assign / remove_if / operator== / operator!=
// - snake_case, One True Brace Style, template params T, Fn

#include "bc_allocator.h"

#include <stddef.h>
#include <assert.h>
#include <new>
#include <utility>
#include <type_traits>

namespace df {

template <typename T>
class list {
    struct node {
        node* prev;
        node* next;
        T     value;
    };

public:
    template <bool IsConst>
    class iter_t {
        using node_ptr  = std::conditional_t<IsConst, const node*, node*>;
        using value_ref = std::conditional_t<IsConst, const T&, T&>;
        using value_pp  = std::conditional_t<IsConst, const T*, T*>;

        node_ptr cur_ = nullptr;

    public:
        using value_type        = T;
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
            cur_ = cur_->next;
            return *this;
        }
        iter_t operator++(int) noexcept {
            iter_t t(*this);
            ++(*this);
            return t;
        }
        iter_t& operator--() noexcept {
            cur_ = cur_->prev;
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
    };

    using iterator       = iter_t<false>;
    using const_iterator = iter_t<true>;

    list() noexcept {}

    list(const list&) = delete;
    list& operator=(const list&) = delete;

    list(list&& other) noexcept {
        steal(other);
    }

    list& operator=(list&& other) noexcept {
        if (this != &other) {
            destroy_all();
            steal(other);
        }
        return *this;
    }

    ~list() {
        destroy_all();
    }

    size_t    size()  const noexcept { return static_cast<size_t>(len_); }
    ptrdiff_t ssize() const noexcept { return len_; }
    bool      empty() const noexcept { return len_ == 0; }

    T& front() noexcept {
        assert(len_ > 0);
        return head_->value;
    }
    const T& front() const noexcept {
        assert(len_ > 0);
        return head_->value;
    }

    T& back() noexcept {
        assert(len_ > 0);
        return tail_->value;
    }
    const T& back() const noexcept {
        assert(len_ > 0);
        return tail_->value;
    }

    iterator begin() noexcept { return iterator(head_); }
    iterator end()   noexcept { return iterator(nullptr); }
    const_iterator begin() const noexcept { return const_iterator(head_); }
    const_iterator end()   const noexcept { return const_iterator(nullptr); }
    const_iterator cbegin() const noexcept { return const_iterator(head_); }
    const_iterator cend()   const noexcept { return const_iterator(nullptr); }

    void push_back(const T& v) {
        node* n = make_node(v);
        link_back(n);
    }
    void push_back(T&& v) {
        node* n = make_node(std::move(v));
        link_back(n);
    }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        node* n = make_node_emplace(std::forward<Args>(args)...);
        link_back(n);
        return n->value;
    }

    void push_front(const T& v) {
        node* n = make_node(v);
        link_front(n);
    }
    void push_front(T&& v) {
        node* n = make_node(std::move(v));
        link_front(n);
    }

    template <typename... Args>
    T& emplace_front(Args&&... args) {
        node* n = make_node_emplace(std::forward<Args>(args)...);
        link_front(n);
        return n->value;
    }

    void pop_back() noexcept {
        assert(len_ > 0);
        unlink_and_destroy(tail_);
    }
    void pop_front() noexcept {
        assert(len_ > 0);
        unlink_and_destroy(head_);
    }

    iterator insert(const_iterator pos, const T& v) {
        node* pos_node = pos.raw();
        node* n = make_node(v);
        link_before(pos_node, n);
        return iterator(n);
    }
    iterator insert(const_iterator pos, T&& v) {
        node* pos_node = pos.raw();
        node* n = make_node(std::move(v));
        link_before(pos_node, n);
        return iterator(n);
    }

    template <typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        node* pos_node = pos.raw();
        node* n = make_node_emplace(std::forward<Args>(args)...);
        link_before(pos_node, n);
        return iterator(n);
    }

    iterator erase(const_iterator pos) noexcept {
        node* n = pos.raw();
        assert(n != nullptr && "list::erase on end()");
        node* next = n->next;
        unlink_and_destroy(n);
        return iterator(next);
    }

    void clear() noexcept {
        node* n = head_;
        while (n != nullptr) {
            node* next = n->next;
            destroy_node(n);
            n = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        len_ = 0;
    }

    void swap(list& other) noexcept {
        node* h = head_;         head_ = other.head_;       other.head_ = h;
        node* t = tail_;         tail_ = other.tail_;       other.tail_ = t;
        ptrdiff_t l = len_;      len_ = other.len_;         other.len_ = l;
    }

    bool contains(const T& value) const {
        for (node* n = head_; n != nullptr; n = n->next) {
            if (n->value == value) {
                return true;
            }
        }
        return false;
    }

    void resize(ptrdiff_t count) {
        resize(count, T{});
    }

    void resize(ptrdiff_t count, const T& value) {
        assert(count >= 0);
        if (count < len_) {
            node* n = head_;
            for (ptrdiff_t i = 0; i < count; ++i) {
                n = n->next;
            }
            while (n != nullptr) {
                node* next = n->next;
                unlink_and_destroy(n);
                n = next;
            }
        } else {
            while (len_ < count) {
                push_back(value);
            }
        }
    }

    template <typename InputIt>
    void assign(InputIt first, InputIt last) {
        clear();
        for (; first != last; ++first) {
            push_back(*first);
        }
    }

    void assign(ptrdiff_t count, const T& value) {
        assert(count >= 0);
        clear();
        for (ptrdiff_t i = 0; i < count; ++i) {
            push_back(value);
        }
    }

    template <typename Pred>
    ptrdiff_t remove_if(Pred pred) {
        ptrdiff_t removed = 0;
        node* n = head_;
        while (n != nullptr) {
            node* next = n->next;
            if (pred(n->value)) {
                unlink_and_destroy(n);
                ++removed;
            }
            n = next;
        }
        return removed;
    }

    bool operator==(const list& other) const {
        if (len_ != other.len_) {
            return false;
        }
        const node* a = head_;
        const node* b = other.head_;
        while (a != nullptr) {
            if (!(a->value == b->value)) {
                return false;
            }
            a = a->next;
            b = b->next;
        }
        return true;
    }

    bool operator!=(const list& other) const {
        return !(*this == other);
    }

    ptrdiff_t remove(const T& v) {
        ptrdiff_t removed = 0;
        node* n = head_;
        while (n != nullptr) {
            node* next = n->next;
            if (n->value == v) {
                unlink_and_destroy(n);
                ++removed;
            }
            n = next;
        }
        return removed;
    }

    void splice(const_iterator pos, list& other) noexcept {
        if (other.empty()) {
            return;
        }
        if (&other == this) {
            return;
        }
        node* pos_node = pos.raw();
        node* first = other.head_;
        node* last  = other.tail_;

        if (pos_node == head_) {
            last->next = head_;
            head_->prev = last;
            head_ = first;
            first->prev = nullptr;
        } else if (pos_node == nullptr) {
            tail_->next = first;
            first->prev = tail_;
            tail_ = last;
            last->next = nullptr;
        } else {
            node* before = pos_node->prev;
            before->next = first;
            first->prev = before;
            last->next = pos_node;
            pos_node->prev = last;
        }
        len_ += other.len_;

        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.len_ = 0;
    }

    void reverse() noexcept {
        if (len_ < 2) {
            return;
        }
        node* n = head_;
        while (n != nullptr) {
            node* next = n->next;
            n->next = n->prev;
            n->prev = next;
            n = next;
        }
        node* h = head_;
        head_ = tail_;
        tail_ = h;
    }

    ptrdiff_t unique() {
        if (len_ < 2) {
            return 0;
        }
        ptrdiff_t removed = 0;
        node* n = head_;
        while (n != nullptr && n->next != nullptr) {
            if (n->value == n->next->value) {
                node* dup = n->next;
                n->next = dup->next;
                if (dup->next != nullptr) {
                    dup->next->prev = n;
                } else {
                    tail_ = n;
                }
                destroy_node(dup);
                --len_;
                ++removed;
            } else {
                n = n->next;
            }
        }
        return removed;
    }

    template <typename Pred>
    ptrdiff_t unique(Pred pred) {
        if (len_ < 2) {
            return 0;
        }
        ptrdiff_t removed = 0;
        node* n = head_;
        while (n != nullptr && n->next != nullptr) {
            if (pred(n->value, n->next->value)) {
                node* dup = n->next;
                n->next = dup->next;
                if (dup->next != nullptr) {
                    dup->next->prev = n;
                } else {
                    tail_ = n;
                }
                destroy_node(dup);
                --len_;
                ++removed;
            } else {
                n = n->next;
            }
        }
        return removed;
    }

    void sort() {
        sort_impl([](const T& a, const T& b) -> bool { return a < b; });
    }

    template <typename Compare>
    void sort(Compare cmp) {
        sort_impl(cmp);
    }

    void merge(list& other) {
        merge_impl(other, [](const T& a, const T& b) -> bool { return a < b; });
    }

    template <typename Compare>
    void merge(list& other, Compare cmp) {
        merge_impl(other, cmp);
    }

    void merge(list&& other) {
        merge(other);
    }

    template <typename Compare>
    void merge(list&& other, Compare cmp) {
        merge(other, cmp);
    }

    iterator find(const T& v) noexcept {
        node* n = head_;
        while (n != nullptr) {
            if (n->value == v) {
                return iterator(n);
            }
            n = n->next;
        }
        return end();
    }
    const_iterator find(const T& v) const noexcept {
        const node* n = head_;
        while (n != nullptr) {
            if (n->value == v) {
                return const_iterator(n);
            }
            n = n->next;
        }
        return end();
    }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (node* n = head_; n != nullptr; n = n->next) {
            fn(n->value);
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const node* n = head_; n != nullptr; n = n->next) {
            fn(n->value);
        }
    }

private:
    node*     head_ = nullptr;
    node*     tail_ = nullptr;
    ptrdiff_t len_  = 0;

    static node* alloc_node() {
        void* raw = df::allocate(sizeof(node));
        assert(raw != nullptr && "list: node allocation failed");
        return static_cast<node*>(raw);
    }

    static void free_node(node* n) noexcept {
        df::deallocate(n, sizeof(node));
    }

    template <typename V>
    node* make_node(V&& v) {
        node* n = alloc_node();
        new (&n->value) T(std::forward<V>(v));
        n->prev = nullptr;
        n->next = nullptr;
        return n;
    }

    template <typename... Args>
    node* make_node_emplace(Args&&... args) {
        node* n = alloc_node();
        new (&n->value) T(std::forward<Args>(args)...);
        n->prev = nullptr;
        n->next = nullptr;
        return n;
    }

    static void destroy_node(node* n) noexcept {
        n->value.~T();
        free_node(n);
    }

    void destroy_all() noexcept {
        clear();
    }

    void steal(list& other) noexcept {
        head_ = other.head_;
        tail_ = other.tail_;
        len_  = other.len_;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.len_ = 0;
    }

    void link_back(node* n) noexcept {
        n->prev = tail_;
        n->next = nullptr;
        if (tail_ != nullptr) {
            tail_->next = n;
        } else {
            head_ = n;
        }
        tail_ = n;
        ++len_;
    }

    void link_front(node* n) noexcept {
        n->next = head_;
        n->prev = nullptr;
        if (head_ != nullptr) {
            head_->prev = n;
        } else {
            tail_ = n;
        }
        head_ = n;
        ++len_;
    }

    void link_before(node* pos_node, node* n) noexcept {
        if (pos_node == nullptr) {
            link_back(n);
            return;
        }
        if (pos_node == head_) {
            link_front(n);
            return;
        }
        node* before = pos_node->prev;
        n->prev = before;
        n->next = pos_node;
        before->next = n;
        pos_node->prev = n;
        ++len_;
    }

    void unlink_and_destroy(node* n) noexcept {
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            head_ = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        } else {
            tail_ = n->prev;
        }
        destroy_node(n);
        --len_;
    }

    template <typename Compare>
    void sort_impl(Compare& cmp) {
        if (len_ < 2) {
            return;
        }

        for (ptrdiff_t width = 1; width < len_; width *= 2) {
            node* cur = head_;
            node* new_head = nullptr;
            node* new_tail = nullptr;
            while (cur != nullptr) {
                node* a = cur;
                node* b = split_run(a, width);
                cur = split_run(b, width);

                node* merged_head = nullptr;
                node* merged_tail = nullptr;
                merge_two(a, b, merged_head, merged_tail, cmp);

                if (new_tail == nullptr) {
                    new_head = merged_head;
                    new_tail = merged_tail;
                } else {
                    new_tail->next = merged_head;
                    merged_head->prev = new_tail;
                    new_tail = merged_tail;
                }
            }
            head_ = new_head;
            tail_ = new_tail;
            tail_->next = nullptr;
        }
    }

    static node* split_run(node* start, ptrdiff_t count) noexcept {
        if (start == nullptr) {
            return nullptr;
        }
        node* n = start;
        ptrdiff_t i = 1;
        while (i < count && n->next != nullptr) {
            n = n->next;
            ++i;
        }
        node* rest = n->next;
        n->next = nullptr;
        if (rest != nullptr) {
            rest->prev = nullptr;
        }
        return rest;
    }

    template <typename Compare>
    static void merge_two(node* a, node* b, node*& out_head, node*& out_tail, Compare& cmp) {
        out_head = nullptr;
        out_tail = nullptr;

        auto append = [&](node* n) {
            n->next = nullptr;
            n->prev = out_tail;
            if (out_tail != nullptr) {
                out_tail->next = n;
            } else {
                out_head = n;
            }
            out_tail = n;
        };

        while (a != nullptr && b != nullptr) {
            if (cmp(b->value, a->value)) {
                append(b);
                b = b->next;
            } else {
                append(a);
                a = a->next;
            }
        }
        while (a != nullptr) {
            append(a);
            a = a->next;
        }
        while (b != nullptr) {
            append(b);
            b = b->next;
        }
    }

    template <typename Compare>
    void merge_impl(list& other, Compare& cmp) {
        if (&other == this || other.empty()) {
            return;
        }
        node* a = head_;
        node* b = other.head_;
        node* new_head = nullptr;
        node* new_tail = nullptr;

        auto append = [&](node* n) {
            n->next = nullptr;
            n->prev = new_tail;
            if (new_tail != nullptr) {
                new_tail->next = n;
            } else {
                new_head = n;
            }
            new_tail = n;
        };

        while (a != nullptr && b != nullptr) {
            if (cmp(b->value, a->value)) {
                append(b);
                b = b->next;
            } else {
                append(a);
                a = a->next;
            }
        }
        while (a != nullptr) {
            append(a);
            a = a->next;
        }
        while (b != nullptr) {
            append(b);
            b = b->next;
        }

        head_ = new_head;
        tail_ = new_tail;
        len_ += other.len_;

        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.len_ = 0;
    }

    template <typename Compare>
    void sort_impl(Compare&& cmp) {
        auto local = std::forward<Compare>(cmp);
        sort_impl(local);
    }

    template <typename Compare>
    void merge_impl(list& other, Compare&& cmp) {
        auto local = std::forward<Compare>(cmp);
        merge_impl(other, local);
    }
};

template <typename T>
inline void swap(list<T>& a, list<T>& b) noexcept {
    a.swap(b);
}

} // namespace df