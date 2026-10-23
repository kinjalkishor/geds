#pragma once

// bc_priority_queue.h
//
// Binary-heap priority queue, wrapper around df::vector<T>.
// Max-heap by default (per std::less<T>). Pass std::greater<T> for min-heap,
// or a lambda for custom ordering on a struct.
//
// Style: snake_case, One True Brace Style, template params T / Compare / Args.

#include "bc_vector.h"

#include <stddef.h>
#include <assert.h>
#include <functional>
#include <utility>

namespace df {

template <typename T, typename Compare = std::less<T>>
class priority_queue {
public:
    using value_type = T;
    using size_type  = size_t;

    priority_queue() = default;

    explicit priority_queue(Compare cmp) : cmp_(std::move(cmp)) {}

    priority_queue(const priority_queue&) = delete;
    priority_queue& operator=(const priority_queue&) = delete;

    priority_queue(priority_queue&& other) noexcept
        : v_(std::move(other.v_)), cmp_(std::move(other.cmp_)) {}

    priority_queue& operator=(priority_queue&& other) noexcept {
        if (this != &other) {
            v_ = std::move(other.v_);
            cmp_ = std::move(other.cmp_);
        }
        return *this;
    }

    ~priority_queue() = default;

    bool      empty()     const noexcept { return v_.empty(); }
    size_t    size()      const noexcept { return v_.size(); }
    ptrdiff_t ssize()     const noexcept { return v_.ssize(); }
    size_t    capacity()  const noexcept { return v_.capacity(); }
    ptrdiff_t scapacity() const noexcept { return v_.scapacity(); }

    void reserve(ptrdiff_t n) { v_.reserve(n); }

    const T& top() const noexcept {
        assert(!v_.empty() && "priority_queue::top on empty");
        return v_.front();
    }

    void push(const T& value) {
        v_.push_back(value);
        sift_up(v_.ssize() - 1);
    }

    void push(T&& value) {
        v_.push_back(std::move(value));
        sift_up(v_.ssize() - 1);
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        v_.emplace_back(std::forward<Args>(args)...);
        const ptrdiff_t idx = v_.ssize() - 1;
        sift_up(idx);
        return v_[idx];
    }

    void pop() {
        assert(!v_.empty() && "priority_queue::pop on empty");
        const ptrdiff_t last = v_.ssize() - 1;
        if (last == 0) {
            v_.pop_back();
            return;
        }
        using std::swap;
        swap(v_[0], v_[last]);
        v_.pop_back();
        sift_down(0);
    }

    void clear() noexcept {
        v_.clear();
    }

    void swap(priority_queue& other) noexcept {
        v_.swap(other.v_);
        using std::swap;
        swap(cmp_, other.cmp_);
    }

private:
    df::vector<T> v_;
    Compare cmp_;

    void sift_up(ptrdiff_t idx) {
        using std::swap;
        while (idx > 0) {
            const ptrdiff_t parent = (idx - 1) / 2;
            if (!cmp_(v_[parent], v_[idx])) {
                return;
            }
            swap(v_[parent], v_[idx]);
            idx = parent;
        }
    }

    void sift_down(ptrdiff_t idx) {
        using std::swap;
        const ptrdiff_t n = v_.ssize();
        while (true) {
            const ptrdiff_t left = 2 * idx + 1;
            if (left >= n) {
                return;
            }
            ptrdiff_t best = left;
            const ptrdiff_t right = left + 1;
            if (right < n && cmp_(v_[left], v_[right])) {
                best = right;
            }
            if (!cmp_(v_[idx], v_[best])) {
                return;
            }
            swap(v_[idx], v_[best]);
            idx = best;
        }
    }
};

template <typename T, typename Compare>
inline void swap(priority_queue<T, Compare>& a, priority_queue<T, Compare>& b) noexcept {
    a.swap(b);
}

} // namespace df