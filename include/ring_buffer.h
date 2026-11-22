#pragma once
#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace falcon {

// Lock-free SPSC Ring Buffer
// Single producer (CTP SPI thread), Single consumer (worker thread)
//
// Slots are default-constructed once and reused via assignment, so element
// lifetimes are always managed by the vector (safe for types holding
// std::string etc.). One slot is kept empty to distinguish full from empty,
// so the usable capacity is Capacity - 1.
template<typename T, size_t Capacity = 65536>
class RingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");

    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
    alignas(64) std::vector<T> buffer_ = std::vector<T>(Capacity);

public:
    RingBuffer() = default;
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    static constexpr size_t capacity() { return Capacity - 1; }

    // Producer only. Arguments are left untouched when the buffer is full.
    template<typename U>
    bool tryPush(U&& value) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t next = (w + 1) & kMask;
        if (next == read_pos_.load(std::memory_order_acquire)) return false;  // Full

        buffer_[w] = std::forward<U>(value);
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool tryPop(T& out) {
        size_t r = read_pos_.load(std::memory_order_relaxed);
        if (r == write_pos_.load(std::memory_order_acquire)) return false;  // Empty

        out = std::move(buffer_[r]);
        read_pos_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const {
        return read_pos_.load(std::memory_order_acquire)
            == write_pos_.load(std::memory_order_acquire);
    }
};

} // namespace falcon
