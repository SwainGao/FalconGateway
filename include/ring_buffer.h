#pragma once
#include <atomic>
#include <vector>
#include <memory>
#include <new>
#include <cstring>

namespace falcon {

// Lock-free SPSC Ring Buffer
// Single producer (CTP SPI thread), Single consumer (worker thread)
template<typename T, size_t Capacity = 65536>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
    alignas(64) std::vector<T> buffer_{Capacity};

public:
    RingBuffer() = default;

    // Producer: CTP SPI thread calls this
    template<typename... Args>
    bool tryPush(Args&&... args) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t r = read_pos_.load(std::memory_order_acquire);
        size_t next = (w + 1) & kMask;

        if (next == r) return false;  // Full

        ::new (&buffer_[w]) T(std::forward<Args>(args)...);
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer: worker thread calls this
    bool tryPop(T& out) {
        size_t r = read_pos_.load(std::memory_order_relaxed);
        size_t w = write_pos_.load(std::memory_order_acquire);

        if (r == w) return false;  // Empty

        out = std::move(buffer_[r]);
        buffer_[r].~T();
        read_pos_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const {
        return read_pos_.load(std::memory_order_acquire)
            == write_pos_.load(std::memory_order_acquire);
    }
};

// Fixed-size pre-allocated buffer for callbacks (avoids heap alloc in SPI thread)
// Suitable for types that are trivially copyable and small
template<typename T, size_t Capacity = 1024>
class FixedQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) size_t read_{0};  // Only consumer accesses
    std::vector<T> buffer_{Capacity};

public:
    bool tryPush(const T& item) {
        size_t w = write_.load(std::memory_order_relaxed);
        size_t next = (w + 1) & kMask;
        if (next == read_) return false;
        buffer_[w] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        if (read_ == write_.load(std::memory_order_acquire)) return false;
        out = buffer_[read_];
        read_ = (read_ + 1) & kMask;
        return true;
    }
};

} // namespace falcon
