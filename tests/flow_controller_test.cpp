#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "flow_controller.h"

using namespace std::chrono_literals;
using falcon::FlowController;

namespace {

// Simulates CTP answering each query shortly after it is sent
struct FakeCtp {
    FlowController& fc;
    std::mutex mtx;
    std::vector<int> ids;
    std::vector<std::chrono::steady_clock::time_point> times;
    std::vector<std::thread> responders;

    explicit FakeCtp(FlowController& f) : fc(f) {}
    ~FakeCtp() { for (auto& t : responders) t.join(); }

    FlowController::Query query(bool respond = true) {
        return [this, respond](int id) {
            {
                std::lock_guard lk(mtx);
                ids.push_back(id);
                times.push_back(std::chrono::steady_clock::now());
                if (respond) {
                    responders.emplace_back([this, id] {
                        std::this_thread::sleep_for(5ms);
                        fc.onResponseComplete(id);
                    });
                }
            }
            return 0;
        };
    }
};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

} // namespace

TEST(FlowController, SerializesQueriesWithMinimumInterval) {
    FlowController fc;
    fc.setInterval(30);
    FakeCtp ctp(fc);
    fc.start();
    for (int i = 0; i < 3; ++i) fc.submit(ctp.query(), "q");
    ASSERT_TRUE(waitFor([&] { return fc.allDone(); }));
    fc.stop();

    ASSERT_EQ(ctp.ids.size(), 3u);
    EXPECT_GE(ctp.ids[0], FlowController::kFirstRequestId);
    for (size_t i = 1; i < ctp.ids.size(); ++i) {
        EXPECT_EQ(ctp.ids[i], ctp.ids[i - 1] + 1);
        EXPECT_GE(ctp.times[i] - ctp.times[i - 1], 29ms);
    }
}

TEST(FlowController, MarkerRunsAfterEarlierQueriesComplete) {
    FlowController fc;
    fc.setInterval(1);
    FakeCtp ctp(fc);
    fc.start();
    std::promise<size_t> seen;
    fc.submit(ctp.query(), "a");
    fc.submit(ctp.query(), "b");
    fc.submitMarker([&] {
        std::lock_guard lk(ctp.mtx);
        seen.set_value(ctp.ids.size());
    });
    auto fut = seen.get_future();
    ASSERT_EQ(fut.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(fut.get(), 2u);
    EXPECT_EQ(fc.inFlightRequestId(), 0);
    fc.stop();
}

TEST(FlowController, MismatchedResponseDoesNotCompleteQuery) {
    FlowController fc;
    fc.setInterval(1);
    FakeCtp ctp(fc);
    fc.start();
    fc.submit(ctp.query(false), "silent");
    ASSERT_TRUE(waitFor([&] { return fc.inFlightRequestId() != 0; }));
    int id = fc.inFlightRequestId();
    EXPECT_FALSE(fc.onResponseComplete(id + 1));
    EXPECT_EQ(fc.inFlightRequestId(), id);
    EXPECT_TRUE(fc.onResponseComplete(id));
    EXPECT_TRUE(waitFor([&] { return fc.allDone(); }));
    fc.stop();
}

TEST(FlowController, TimeoutFreesTheQueue) {
    FlowController fc;
    fc.setInterval(1);
    fc.setTimeout(50);
    FakeCtp ctp(fc);
    fc.start();
    fc.submit(ctp.query(false), "lost");
    fc.submit(ctp.query(), "next");
    ASSERT_TRUE(waitFor([&] { return fc.allDone(); }));
    EXPECT_EQ(fc.timeoutCount(), 1);
    EXPECT_EQ(ctp.ids.size(), 2u);
    fc.stop();
}

TEST(FlowController, RetriesWhenReqReturnsError) {
    FlowController fc;
    fc.setInterval(1);
    fc.start();
    std::atomic<int> calls{0};
    fc.submit([&](int id) {
        if (++calls == 1) return -3;  // CTP: too many requests per second
        fc.onResponseComplete(id);
        return 0;
    }, "retry");
    ASSERT_TRUE(waitFor([&] { return fc.allDone() && calls == 2; }));
    fc.stop();
}

TEST(FlowController, ResetDropsPendingWork) {
    FlowController fc;
    fc.setInterval(1);
    FakeCtp ctp(fc);
    fc.start();
    fc.submit(ctp.query(false), "stuck");
    ASSERT_TRUE(waitFor([&] { return fc.inFlightRequestId() != 0; }));
    std::atomic<bool> marker_ran{false};
    fc.submit(ctp.query(), "dropped");
    fc.submitMarker([&] { marker_ran = true; });
    fc.reset();
    EXPECT_TRUE(waitFor([&] { return fc.allDone(); }));
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(marker_ran);
    EXPECT_EQ(ctp.ids.size(), 1u);
    fc.stop();
}
