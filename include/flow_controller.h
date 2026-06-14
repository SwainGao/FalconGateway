#pragma once
#include <queue>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include "types.h"

namespace falcon {

// Manages CTP query flow control:
// - Only 1 in-flight query at a time
// - Minimum interval between queries (default 1s)
class FlowController {
    std::queue<std::function<void()>> pending_;
    std::atomic<bool> in_flight_{false};
    Timestamp last_query_;
    int min_interval_ms_ = 1100;  // 1.1s to be safe

public:
    void setInterval(int ms) { min_interval_ms_ = ms; }

    // Submit a query. If another is in flight, queue it.
    // Returns true if it was sent immediately
    bool submit(std::function<void()> query) {
        if (in_flight_.exchange(true)) {
            pending_.push(std::move(query));
            return false;
        }
        execute(std::move(query));
        return true;
    }

    // Must be called when OnRspQry* with bIsLast==true arrives
    void onResponseComplete() {
        in_flight_ = false;
        if (!pending_.empty()) {
            auto next = std::move(pending_.front());
            pending_.pop();
            submit(std::move(next));
        }
    }

    [[nodiscard]] bool allDone() const {
        return !in_flight_ && pending_.empty();
    }

    void reset() {
        in_flight_ = false;
        while (!pending_.empty()) pending_.pop();
    }

private:
    void execute(std::function<void()> query) {
        auto now = falcon::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_query_).count();
        if (elapsed < min_interval_ms_) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(min_interval_ms_ - elapsed));
        }
        last_query_ = falcon::now();
        query();
    }
};

} // namespace falcon
