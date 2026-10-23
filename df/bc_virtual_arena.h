#pragma once

// bc_virtual_arena.h
//
// Reserve-then-commit arena for large streamed assets. Uses the OS's
// virtual memory APIs (VirtualAlloc on Windows, mmap/mprotect on POSIX)
// to reserve address space at startup and commit physical pages only as
// data is allocated.
//
// Model:
//   - reserve(capacity)    one OS call at startup
//   - allocate(size)       bump pointer, commit pages on demand
//   - decommit(cp)         return physical pages to the OS from cp
//   - reset()              reset the marker without decommitting
//   - reset_and_decommit() reset marker and decommit all committed pages
//   - release()            release the virtual address space entirely
//
// Style: snake_case, One True Brace Style.

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <sys/mman.h>
#   include <unistd.h>
#endif

namespace df {

class virtual_arena {
public:
    using checkpoint = ptrdiff_t;

    virtual_arena() noexcept = default;

    explicit virtual_arena(size_t capacity) noexcept {
        reserve(capacity);
    }

    virtual_arena(const virtual_arena&) = delete;
    virtual_arena& operator=(const virtual_arena&) = delete;

    virtual_arena(virtual_arena&&) = delete;
    virtual_arena& operator=(virtual_arena&&) = delete;

    ~virtual_arena() {
        release();
    }

    bool reserve(size_t capacity) noexcept {
        if (base_ != nullptr) {
            release();
        }
        if (capacity == 0) {
            return false;
        }
        page_size_ = query_page_size();
        assert(page_size_ > 0);

        const size_t rounded = round_up(capacity, page_size_);

#if defined(_WIN32)
        void* p = VirtualAlloc(nullptr, rounded, MEM_RESERVE, PAGE_NOACCESS);
        if (p == nullptr) {
            return false;
        }
#else
        void* p = mmap(nullptr, rounded,
                       PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                       -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
#endif
        base_ = static_cast<unsigned char*>(p);
        capacity_ = static_cast<ptrdiff_t>(rounded);
        offset_marker_ = 0;
        committed_marker_ = 0;
        return true;
    }

    void release() noexcept {
        if (base_ == nullptr) {
            return;
        }
#if defined(_WIN32)
        VirtualFree(base_, 0, MEM_RELEASE);
#else
        munmap(base_, static_cast<size_t>(capacity_));
#endif
        base_ = nullptr;
        capacity_ = 0;
        offset_marker_ = 0;
        committed_marker_ = 0;
    }

    void* allocate(size_t size, size_t align = 0) noexcept {
        return try_allocate(size, align);
    }

    void* try_allocate(size_t size, size_t align = 0) noexcept {
        if (base_ == nullptr) {
            return nullptr;
        }
        if (size == 0) {
            size = 1;
        }
        if (align == 0 || align < page_size_) {
            align = page_size_;
        }
        if ((align & (align - 1)) != 0) {
            return nullptr;
        }

        const ptrdiff_t misalignment = offset_marker_ & (static_cast<ptrdiff_t>(align) - 1);
        const ptrdiff_t padding = misalignment ? (static_cast<ptrdiff_t>(align) - misalignment) : 0;
        const ptrdiff_t need = padding + static_cast<ptrdiff_t>(size);

        if (offset_marker_ + need > capacity_) {
            return nullptr;
        }

        const ptrdiff_t new_end = offset_marker_ + need;
        if (new_end > committed_marker_) {
            const ptrdiff_t new_committed = round_up_ptrdiff(new_end, page_size_);
            if (!os_commit(committed_marker_, new_committed - committed_marker_)) {
                return nullptr;
            }
            committed_marker_ = new_committed;
        }

        offset_marker_ += padding;
        void* p = base_ + offset_marker_;
        offset_marker_ += static_cast<ptrdiff_t>(size);
        return p;
    }

    bool commit(size_t extra_bytes) noexcept {
        if (base_ == nullptr) {
            return false;
        }
        const ptrdiff_t new_end = offset_marker_ + static_cast<ptrdiff_t>(extra_bytes);
        if (new_end > capacity_) {
            return false;
        }
        if (new_end <= committed_marker_) {
            return true;
        }
        const ptrdiff_t new_committed = round_up_ptrdiff(new_end, page_size_);
        if (!os_commit(committed_marker_, new_committed - committed_marker_)) {
            return false;
        }
        committed_marker_ = new_committed;
        return true;
    }

    bool decommit(checkpoint cp) noexcept {
        if (base_ == nullptr) {
            return false;
        }
        const ptrdiff_t aligned_cp = round_down_ptrdiff(cp, page_size_);
        if (aligned_cp >= committed_marker_) {
            return true;
        }
        if (aligned_cp < 0) {
            return false;
        }
        if (!os_decommit(aligned_cp, committed_marker_ - aligned_cp)) {
            return false;
        }
        committed_marker_ = aligned_cp;
        return true;
    }

    bool rewind_and_decommit(checkpoint cp) noexcept {
        if (base_ == nullptr) {
            return false;
        }
        if (cp < 0 || cp > offset_marker_) {
            return false;
        }
        const bool ok = decommit(cp);
        offset_marker_ = cp;
        return ok;
    }

    checkpoint get_checkpoint() const noexcept {
        return offset_marker_;
    }

    void rewind(checkpoint cp) noexcept {
        assert(cp >= 0 && cp <= offset_marker_ && "invalid checkpoint");
        offset_marker_ = cp;
    }

    void reset() noexcept {
        offset_marker_ = 0;
    }

    void reset_and_decommit() noexcept {
        offset_marker_ = 0;
        if (committed_marker_ > 0) {
            os_decommit(0, committed_marker_);
            committed_marker_ = 0;
        }
    }

    ptrdiff_t ssize() const noexcept {
        return offset_marker_;
    }

    size_t size() const noexcept {
        return static_cast<size_t>(offset_marker_);
    }

    ptrdiff_t scapacity() const noexcept {
        return capacity_;
    }

    size_t capacity() const noexcept {
        return static_cast<size_t>(capacity_);
    }

    size_t committed() const noexcept {
        return static_cast<size_t>(committed_marker_);
    }

    size_t page_size() const noexcept {
        return page_size_;
    }

    void* data() noexcept {
        return base_;
    }

    const void* data() const noexcept {
        return base_;
    }

private:
    unsigned char* base_ = nullptr;
    ptrdiff_t capacity_ = 0;
    ptrdiff_t offset_marker_ = 0;
    ptrdiff_t committed_marker_ = 0;
    size_t page_size_ = 0;

    static size_t round_up(size_t v, size_t align) noexcept {
        return (v + align - 1) & ~(align - 1);
    }

    static ptrdiff_t round_up_ptrdiff(ptrdiff_t v, size_t align) noexcept {
        const ptrdiff_t a = static_cast<ptrdiff_t>(align);
        return (v + a - 1) & ~(a - 1);
    }

    static ptrdiff_t round_down_ptrdiff(ptrdiff_t v, size_t align) noexcept {
        const ptrdiff_t a = static_cast<ptrdiff_t>(align);
        return v & ~(a - 1);
    }

    static size_t query_page_size() noexcept {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<size_t>(si.dwPageSize);
#else
        long p = sysconf(_SC_PAGESIZE);
        if (p <= 0) {
            return 4096;
        }
        return static_cast<size_t>(p);
#endif
    }

    bool os_commit(ptrdiff_t offset, ptrdiff_t size) noexcept {
        if (size <= 0) {
            return true;
        }
#if defined(_WIN32)
        void* p = VirtualAlloc(base_ + offset,
                               static_cast<SIZE_T>(size),
                               MEM_COMMIT,
                               PAGE_READWRITE);
        return p != nullptr;
#else
        int rc = mprotect(base_ + offset,
                          static_cast<size_t>(size),
                          PROT_READ | PROT_WRITE);
        return rc == 0;
#endif
    }

    bool os_decommit(ptrdiff_t offset, ptrdiff_t size) noexcept {
        if (size <= 0) {
            return true;
        }
#if defined(_WIN32)
        BOOL ok = VirtualFree(base_ + offset,
                              static_cast<SIZE_T>(size),
                              MEM_DECOMMIT);
        return ok != 0;
#else
        void* addr = base_ + offset;
        size_t len = static_cast<size_t>(size);
        int adv = madvise(addr, len, MADV_DONTNEED);
        int prot = mprotect(addr, len, PROT_NONE);
        return adv == 0 && prot == 0;
#endif
    }
};

} // namespace df