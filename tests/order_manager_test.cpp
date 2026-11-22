#include <gtest/gtest.h>
#include "order_manager.h"

using namespace falcon;

namespace {

OrderRequest makeReq(int volume = 2) {
    OrderRequest r;
    r.instrument = "rb2607";
    r.exchange = "SHFE";
    r.direction = Direction::Buy;
    r.offset = Offset::Open;
    r.price = 3000;
    r.volume = volume;
    return r;
}

OrderRecord rtn(const char* ref, OrderStatus st, int traded, int total, const char* sys_id = "") {
    OrderRecord r;
    r.front_id = 1;
    r.session_id = 2;
    std::strncpy(r.order_ref.data(), ref, r.order_ref.size() - 1);
    std::strncpy(r.order_sys_id.data(), sys_id, r.order_sys_id.size() - 1);
    r.status = st;
    r.volume_traded = traded;
    r.volume_total = total;
    r.request = makeReq();
    return r;
}

} // namespace

TEST(OrderManager, RejectsDuplicateCreate) {
    OrderManager om;
    EXPECT_TRUE(om.create(makeReq(), 1, 2, "7"));
    EXPECT_FALSE(om.create(makeReq(), 1, 2, "7"));
    EXPECT_EQ(om.size(), 1u);
    EXPECT_TRUE(om.findByKey(OrderManager::makeKey(1, 2, "7")).has_value());
}

TEST(OrderManager, NewlyFinalReportedOnce) {
    OrderManager om;
    om.create(makeReq(), 1, 2, "7");
    auto r1 = om.update(rtn("7", OrderStatus::Canceled, 0, 2, "SYS1"));
    EXPECT_TRUE(r1.found);
    EXPECT_TRUE(r1.newly_final);
    auto r2 = om.update(rtn("7", OrderStatus::Canceled, 0, 2, "SYS1"));
    EXPECT_TRUE(r2.found);
    EXPECT_TRUE(r2.ignored);
    EXPECT_FALSE(r2.newly_final);
}

TEST(OrderManager, IgnoresStaleUpdatesAfterProgress) {
    OrderManager om;
    om.create(makeReq(), 1, 2, "7");
    om.update(rtn("7", OrderStatus::PartTradedQueueing, 1, 1, "SYS1"));
    auto stale = om.update(rtn("7", OrderStatus::NoTradeQueueing, 0, 2, "SYS1"));
    EXPECT_TRUE(stale.ignored);
    EXPECT_EQ(stale.record.volume_traded, 1);
    // Replay of a final state after the order already completed
    om.update(rtn("7", OrderStatus::AllTraded, 2, 0, "SYS1"));
    auto replay = om.update(rtn("7", OrderStatus::NoTradeQueueing, 0, 2, "SYS1"));
    EXPECT_TRUE(replay.ignored);
    EXPECT_EQ(replay.record.status, OrderStatus::AllTraded);
}

TEST(OrderManager, NotQueueingStatusesAreFinal) {
    OrderManager om;
    om.create(makeReq(), 1, 2, "8");
    auto r = om.update(rtn("8", OrderStatus::PartTradedNotQueueing, 1, 1, "SYS2"));
    EXPECT_TRUE(r.newly_final);
    EXPECT_TRUE(om.activeOrders().empty());
}

TEST(OrderManager, LooksUpBySysIdPerExchange) {
    OrderManager om;
    om.create(makeReq(), 1, 2, "9");
    om.update(rtn("9", OrderStatus::NoTradeQueueing, 0, 2, "  12345"));
    auto key = om.keyByOrderSysID("SHFE", "  12345");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, OrderManager::makeKey(1, 2, "9"));
    EXPECT_FALSE(om.keyByOrderSysID("DCE", "  12345").has_value());

    // An update carrying only the sys id still finds the order
    OrderRecord by_sys = rtn("", OrderStatus::Canceled, 0, 2, "  12345");
    by_sys.front_id = 99;
    auto r = om.update(by_sys);
    EXPECT_TRUE(r.found);
    EXPECT_TRUE(r.newly_final);
}

TEST(OrderManager, UnknownOrdersAreNotFound) {
    OrderManager om;
    auto r = om.update(rtn("1", OrderStatus::Canceled, 0, 1, "X"));
    EXPECT_FALSE(r.found);
}

TEST(OrderManager, RemoveDropsOrder) {
    OrderManager om;
    om.create(makeReq(), 1, 2, "5");
    auto key = OrderManager::makeKey(1, 2, "5");
    EXPECT_TRUE(om.remove(key));
    EXPECT_FALSE(om.findByKey(key).has_value());
    EXPECT_FALSE(om.remove(key));
}
