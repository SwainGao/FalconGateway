#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include "ring_buffer.h"

using falcon::RingBuffer;

TEST(RingBuffer, PushPopWrapsAroundWithStrings) {
    RingBuffer<std::string, 4> rb;
    std::string out;
    for (int round = 0; round < 100; ++round) {
        ASSERT_TRUE(rb.tryPush(std::string(64, 'a' + round % 26)));
        ASSERT_TRUE(rb.tryPush("x" + std::to_string(round)));
        ASSERT_TRUE(rb.tryPop(out));
        EXPECT_EQ(out, std::string(64, 'a' + round % 26));
        ASSERT_TRUE(rb.tryPop(out));
        EXPECT_EQ(out, "x" + std::to_string(round));
    }
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.tryPop(out));
}

TEST(RingBuffer, FullPushLeavesValueUntouched) {
    RingBuffer<std::string, 4> rb;
    for (size_t i = 0; i < rb.capacity(); ++i) ASSERT_TRUE(rb.tryPush(std::string("v")));
    std::string keep = "must survive";
    EXPECT_FALSE(rb.tryPush(std::move(keep)));
    EXPECT_EQ(keep, "must survive");
}

TEST(RingBuffer, PoppedSlotsDoNotKeepObjectsAlive) {
    auto p = std::make_shared<int>(42);
    {
        RingBuffer<std::shared_ptr<int>, 8> rb;
        for (int i = 0; i < 5; ++i) ASSERT_TRUE(rb.tryPush(p));
        EXPECT_EQ(p.use_count(), 6);
        std::shared_ptr<int> out;
        while (rb.tryPop(out)) {}
        out.reset();
        EXPECT_EQ(p.use_count(), 1);
        ASSERT_TRUE(rb.tryPush(p));
        EXPECT_EQ(p.use_count(), 2);
    }
    EXPECT_EQ(p.use_count(), 1);  // buffer destruction releases remaining items exactly once
}

TEST(RingBuffer, SpscPreservesOrderAcrossThreads) {
    RingBuffer<std::string, 256> rb;
    constexpr int kCount = 50000;
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            std::string v = std::to_string(i);
            while (!rb.tryPush(std::move(v))) std::this_thread::yield();
        }
    });
    std::string out;
    for (int i = 0; i < kCount; ++i) {
        while (!rb.tryPop(out)) std::this_thread::yield();
        ASSERT_EQ(out, std::to_string(i));
    }
    producer.join();
    EXPECT_TRUE(rb.empty());
}
