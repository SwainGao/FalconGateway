#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "types.h"

namespace falcon {

// Serializes CTP queries on a dedicated thread:
// - Only 1 in-flight query at a time (identified by its request id)
// - Minimum interval between query sends (default 1.1s)
// - A query is complete when the SPI reports bIsLast/error for its request id,
//   or when the timeout expires
// - Queries whose Req* call returns non-zero (CTP flow control -2/-3) are retried
// - Markers run in submission order once all earlier queries have completed
class FlowController {
public:
    // Receives the request id to pass to Req*; returns the Req* return code.
    using Query = std::function<int(int request_id)>;
    using Marker = std::function<void()>;

    // Query request ids start high so they don't collide with order request ids.
    static constexpr int kFirstRequestId = 1 << 24;

    FlowController() = default;
    ~FlowController();
    FlowController(const FlowController&) = delete;
    FlowController& operator=(const FlowController&) = delete;

    void setInterval(int ms);
    void setTimeout(int ms);
    void setMaxRetries(int n);

    void start();
    void stop();

    void submit(Query query, std::string name = {});
    void submitMarker(Marker marker);

    // Called from the SPI thread when a response with bIsLast (or an error)
    // arrives. Returns true if it completed the in-flight query.
    bool onResponseComplete(int request_id);

    // Drops queued queries/markers and forgets the in-flight query.
    void reset();

    [[nodiscard]] bool allDone() const;
    [[nodiscard]] int inFlightRequestId() const;
    [[nodiscard]] int timeoutCount() const;

private:
    struct Item {
        Query query;
        Marker marker;
        std::string name;
        int retries = 0;
    };

    void run();

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Item> pending_;
    std::thread thread_;
    bool running_ = false;
    bool busy_ = false;             // executing a query/marker outside the lock
    int in_flight_id_ = 0;          // 0 = none
    Timestamp in_flight_deadline_{};
    int next_id_ = kFirstRequestId;
    Timestamp last_query_{};
    bool has_last_query_ = false;
    unsigned generation_ = 0;       // bumped by reset()
    int timeouts_ = 0;

    int interval_ms_ = 1100;
    int timeout_ms_ = 10000;
    int max_retries_ = 3;
};

} // namespace falcon
