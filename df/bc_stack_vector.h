#pragma once

// bc_stack_vector.h
//
// Fixed-capacity array with a length marker, allocated inline on the stack.
// Model: T ptr[N]; — a bounded array plus a "used prefix" length. Matches
// std::array<T, N> exactly in construction/destruction semantics.
//
// Style: snake_case, One True Brace Style, template params T, N.

#include <stddef.h>
#include <assert.h>
#include <utility>

namespace df {

template <typename T, int N>
class stack_vector {
public:
    T ptr[N];
    int len = 0;
    static constexpr int cap = N;

    T*       data()       noexcept { return ptr; }
    const T* data() const noexcept { return ptr; }

    size_t size()     const noexcept { return static_cast<size_t>(len); }
    size_t ssize()    const noexcept { return static_cast<size_t>(len); }
    size_t capacity() const noexcept { return static_cast<size_t>(cap); }
    bool   empty()    const noexcept { return len == 0; }
    bool   full()     const noexcept { return len == cap; }

    T*       begin()       noexcept { return ptr; }
    T*       end()         noexcept { return ptr + len; }
    const T* begin() const noexcept { return ptr; }
    const T* end()   const noexcept { return ptr + len; }
    const T* cbegin() const noexcept { return ptr; }
    const T* cend()   const noexcept { return ptr + len; }

    T& operator[](int i) noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }
    const T& operator[](int i) const noexcept {
        assert(i >= 0 && i < len);
        return ptr[i];
    }

    T& at(int i) noexcept {
        assert(i >= 0 && i < len && "stack_vector::at out of range");
        return ptr[i];
    }
    const T& at(int i) const noexcept {
        assert(i >= 0 && i < len && "stack_vector::at out of range");
        return ptr[i];
    }

    T& front() noexcept { assert(len > 0); return ptr[0]; }
    const T& front() const noexcept { assert(len > 0); return ptr[0]; }

    T& back() noexcept { assert(len > 0); return ptr[len - 1]; }
    const T& back() const noexcept { assert(len > 0); return ptr[len - 1]; }

    bool contains(const T& v) const {
        for (int i = 0; i < len; ++i) {
            if (ptr[i] == v) {
                return true;
            }
        }
        return false;
    }

    T* find(const T& v) {
        for (int i = 0; i < len; ++i) {
            if (ptr[i] == v) {
                return &ptr[i];
            }
        }
        return end();
    }
    const T* find(const T& v) const {
        for (int i = 0; i < len; ++i) {
            if (ptr[i] == v) {
                return &ptr[i];
            }
        }
        return end();
    }

    bool push_back(const T& v) {
        if (len >= cap) {
            assert(false && "stack_vector::push_back on full");
            return false;
        }
        ptr[len] = v;
        ++len;
        return true;
    }

    bool push_back(T&& v) {
        if (len >= cap) {
            assert(false && "stack_vector::push_back on full");
            return false;
        }
        ptr[len] = std::move(v);
        ++len;
        return true;
    }

    template <typename... Args>
    T* emplace_back(Args&&... args) {
        if (len >= cap) {
            assert(false && "stack_vector::emplace_back on full");
            return nullptr;
        }
        ptr[len] = T(std::forward<Args>(args)...);
        ++len;
        return &ptr[len - 1];
    }

    void pop_back() {
        if (len <= 0) {
            assert(false && "stack_vector::pop_back on empty");
            return;
        }
        --len;
    }

    bool insert(int pos, const T& v) {
        if (pos < 0 || pos > len) {
            assert(false && "stack_vector::insert pos out of range");
            return false;
        }
        if (len >= cap) {
            assert(false && "stack_vector::insert on full");
            return false;
        }
        for (int i = len; i > pos; --i) {
            ptr[i] = std::move(ptr[i - 1]);
        }
        ptr[pos] = v;
        ++len;
        return true;
    }

    bool insert(int pos, T&& v) {
        if (pos < 0 || pos > len) {
            assert(false && "stack_vector::insert pos out of range");
            return false;
        }
        if (len >= cap) {
            assert(false && "stack_vector::insert on full");
            return false;
        }
        for (int i = len; i > pos; --i) {
            ptr[i] = std::move(ptr[i - 1]);
        }
        ptr[pos] = std::move(v);
        ++len;
        return true;
    }

    void erase(int pos) {
        if (pos < 0 || pos >= len) {
            assert(false && "stack_vector::erase pos out of range");
            return;
        }
        for (int i = pos; i < len - 1; ++i) {
            ptr[i] = std::move(ptr[i + 1]);
        }
        --len;
    }

    void erase(int first, int last) {
        if (first < 0 || first > len || last < first || last > len) {
            assert(false && "stack_vector::erase range out of bounds");
            return;
        }
        if (first == last) {
            return;
        }
        const int shift = last - first;
        for (int i = first; i < len - shift; ++i) {
            ptr[i] = std::move(ptr[i + shift]);
        }
        len -= shift;
    }

    void clear() noexcept {
        len = 0;
    }

    void resize(int new_len) {
        if (new_len < 0) {
            assert(false && "stack_vector::resize with negative len");
            new_len = 0;
        }
        if (new_len > cap) {
            assert(false && "stack_vector::resize past capacity");
            new_len = cap;
        }
        if (new_len > len) {
            for (int i = len; i < new_len; ++i) {
                ptr[i] = T{};
            }
        }
        len = new_len;
    }

    void resize(int new_len, const T& value) {
        if (new_len < 0) {
            assert(false && "stack_vector::resize with negative len");
            new_len = 0;
        }
        if (new_len > cap) {
            assert(false && "stack_vector::resize past capacity");
            new_len = cap;
        }
        if (new_len > len) {
            for (int i = len; i < new_len; ++i) {
                ptr[i] = value;
            }
        }
        len = new_len;
    }

    void assign(int n, const T& value) {
        if (n < 0) {
            assert(false && "stack_vector::assign with negative count");
            n = 0;
        }
        if (n > cap) {
            assert(false && "stack_vector::assign past capacity");
            n = cap;
        }
        for (int i = 0; i < n; ++i) {
            ptr[i] = value;
        }
        len = n;
    }

    template <typename InputIt>
    void assign(InputIt first, InputIt last) {
        int i = 0;
        for (; first != last; ++first) {
            if (i >= cap) {
                assert(false && "stack_vector::assign range past capacity");
                break;
            }
            ptr[i++] = *first;
        }
        len = i;
    }

    bool operator==(const stack_vector& other) const {
        if (len != other.len) {
            return false;
        }
        for (int i = 0; i < len; ++i) {
            if (!(ptr[i] == other.ptr[i])) {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const stack_vector& other) const {
        return !(*this == other);
    }
};

template <typename T, int N>
inline void swap(stack_vector<T, N>& a, stack_vector<T, N>& b) noexcept {
    stack_vector<T, N> tmp = std::move(a);
    a = std::move(b);
    b = std::move(tmp);
}

} // namespace df