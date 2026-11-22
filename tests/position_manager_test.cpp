#include <gtest/gtest.h>
#include "position_manager.h"

using namespace falcon;

namespace {

constexpr double kEps = 1e-6;

InstrumentInfo makeInst(const std::string& id, const std::string& product,
                        const std::string& exch, char date_type) {
    InstrumentInfo i;
    i.instrument_id = id;
    i.product_id = product;
    i.exchange_id = exch;
    i.volume_multiple = 10;
    i.long_margin_ratio = 0.1;
    i.short_margin_ratio = 0.1;
    i.position_date_type = date_type;
    return i;
}

OrderRequest req(const std::string& inst, Direction d, Offset o, double px, int vol) {
    OrderRequest r;
    r.instrument = inst;
    r.exchange = "SHFE";
    r.direction = d;
    r.offset = o;
    r.price = px;
    r.volume = vol;
    return r;
}

TradeRecord fill(const std::string& inst, Direction d, Offset o, double px, int vol) {
    TradeRecord t;
    t.instrument = inst;
    t.direction = d;
    t.offset = o;
    t.price = px;
    t.volume = vol;
    return t;
}

PositionRecord pos(const std::string& inst, char dir, char date, int position, int today) {
    PositionRecord p;
    p.instrument = inst;
    p.posi_direction = dir;
    p.position_date = date;
    p.position = position;
    p.today_position = today;
    p.position_cost = 3000.0 * 10 * position;
    p.open_cost = p.position_cost;
    return p;
}

class PositionManagerTest : public ::testing::Test {
protected:
    PositionManager pm;

    void SetUp() override {
        pm.addInstrument(makeInst("rb2607", "rb", "SHFE", '2'));
        pm.addInstrument(makeInst("v2609", "v", "DCE", '1'));
        CommissionInfo c;
        c.open_ratio_by_volume = 1.0;
        c.close_ratio_by_volume = 1.0;
        c.close_today_ratio_by_volume = 2.0;
        pm.setCommission("rb2607", c);
        pm.setCommission("v", c);  // product-level rate
        pm.setBrokerMarginPriceType('4');  // margin at order/open price

        AccountRecord acc;
        acc.pre_balance = 1'000'000;
        acc.balance = 1'000'000;
        acc.available = 1'000'000;
        pm.onQueryAccount(acc);
    }
};

} // namespace

TEST_F(PositionManagerTest, OpenFreezeFillAndCancelRelease) {
    ASSERT_TRUE(pm.onOrderRequest("k1", req("rb2607", Direction::Buy, Offset::Open, 3000, 2)));
    auto acc = pm.getAccount();
    EXPECT_NEAR(acc.frozen_margin, 6000, kEps);
    EXPECT_NEAR(acc.frozen_commission, 2, kEps);
    EXPECT_NEAR(acc.available, 1'000'000 - 6002, kEps);

    pm.onTradeFill(fill("rb2607", Direction::Buy, Offset::Open, 3000, 1), "k1");
    acc = pm.getAccount();
    EXPECT_NEAR(acc.frozen_margin, 3000, kEps);
    EXPECT_NEAR(acc.frozen_commission, 1, kEps);
    EXPECT_NEAR(acc.curr_margin, 3000, kEps);
    EXPECT_NEAR(acc.commission, 1, kEps);
    auto p = pm.getPosition("rb2607", '2', '1');
    ASSERT_TRUE(p);
    EXPECT_EQ(p->position, 1);
    EXPECT_EQ(p->long_frozen, 1);

    pm.releaseOrder("k1", 1);
    acc = pm.getAccount();
    EXPECT_NEAR(acc.frozen_margin, 0, kEps);
    EXPECT_NEAR(acc.frozen_commission, 0, kEps);
    EXPECT_EQ(pm.getPosition("rb2607", '2', '1')->long_frozen, 0);
    EXPECT_FALSE(pm.getOrderFreeze("k1").has_value());
}

TEST_F(PositionManagerTest, FillDoesNotReleaseOtherOrdersReservation) {
    ASSERT_TRUE(pm.onOrderRequest("k1", req("rb2607", Direction::Buy, Offset::Open, 3000, 1)));
    ASSERT_TRUE(pm.onOrderRequest("k2", req("rb2607", Direction::Buy, Offset::Open, 3000, 1)));
    // Fill below the order price: the residual belongs to k1 only
    pm.onTradeFill(fill("rb2607", Direction::Buy, Offset::Open, 2990, 1), "k1");

    auto acc = pm.getAccount();
    EXPECT_NEAR(acc.frozen_margin, 3000, kEps);
    EXPECT_NEAR(acc.frozen_commission, 1, kEps);
    auto p = pm.getPosition("rb2607", '2', '1');
    EXPECT_EQ(p->long_frozen, 1);
    EXPECT_NEAR(p->frozen_margin, 3000, kEps);
    EXPECT_TRUE(pm.getOrderFreeze("k2").has_value());
}

TEST_F(PositionManagerTest, ShfeCloseFreezesOnlyTheTargetedDateBucket) {
    pm.onQueryPosition(pos("rb2607", '2', '2', 3, 0));  // yesterday
    pm.onQueryPosition(pos("rb2607", '2', '1', 2, 2));  // today

    ASSERT_TRUE(pm.onOrderRequest("ct", req("rb2607", Direction::Sell, Offset::CloseToday, 3000, 2)));
    EXPECT_EQ(pm.getPosition("rb2607", '2', '1')->short_frozen, 2);
    EXPECT_EQ(pm.getPosition("rb2607", '2', '2')->short_frozen, 0);

    ASSERT_TRUE(pm.onOrderRequest("cy", req("rb2607", Direction::Sell, Offset::Close, 3000, 1)));
    EXPECT_EQ(pm.getPosition("rb2607", '2', '2')->short_frozen, 1);
    EXPECT_EQ(pm.getPosition("rb2607", '2', '1')->short_frozen, 2);

    // Commission frozen once per order: 2 lots × 2 (close-today) + 1 lot × 1
    EXPECT_NEAR(pm.getAccount().frozen_commission, 5, kEps);

    pm.releaseOrder("ct", 2);
    pm.releaseOrder("cy", 1);
    EXPECT_EQ(pm.getPosition("rb2607", '2', '1')->short_frozen, 0);
    EXPECT_EQ(pm.getPosition("rb2607", '2', '2')->short_frozen, 0);
    EXPECT_NEAR(pm.getAccount().frozen_commission, 0, kEps);
}

TEST_F(PositionManagerTest, CloseSplitsAcrossBucketsByAvailability) {
    pm.onQueryPosition(pos("rb2607", '3', '2', 1, 0));
    pm.onQueryPosition(pos("rb2607", '3', '1', 2, 2));
    // SHFE "Close" = yesterday first, then today for the remainder
    ASSERT_TRUE(pm.onOrderRequest("c", req("rb2607", Direction::Buy, Offset::Close, 3000, 3)));
    EXPECT_EQ(pm.getPosition("rb2607", '3', '2')->long_frozen, 1);
    EXPECT_EQ(pm.getPosition("rb2607", '3', '1')->long_frozen, 2);
    auto fr = pm.getOrderFreeze("c");
    ASSERT_TRUE(fr);
    int total = 0;
    for (auto& b : fr->buckets) total += b.second;
    EXPECT_EQ(total, 3);
}

TEST_F(PositionManagerTest, CloseFillReleasesFreezeAndUpdatesPosition) {
    pm.onQueryPosition(pos("v2609", '2', '1', 3, 1));
    ASSERT_TRUE(pm.onOrderRequest("c", req("v2609", Direction::Sell, Offset::Close, 3000, 2)));
    EXPECT_EQ(pm.getPosition("v2609", '2', '1')->short_frozen, 2);
    EXPECT_NEAR(pm.getAccount().frozen_commission, 2, kEps);  // product-level rate "v"

    pm.onTradeFill(fill("v2609", Direction::Sell, Offset::Close, 3000, 2), "c");
    auto p = pm.getPosition("v2609", '2', '1');
    EXPECT_EQ(p->position, 1);
    EXPECT_EQ(p->short_frozen, 0);
    EXPECT_NEAR(pm.getAccount().frozen_commission, 0, kEps);
    EXPECT_FALSE(pm.getOrderFreeze("c").has_value());
}

TEST_F(PositionManagerTest, RejectsWhenFundsInsufficient) {
    // 1000 lots × 3000 × 10 × 10% = 3,000,000 > 1,000,000 available
    EXPECT_FALSE(pm.onOrderRequest("big", req("rb2607", Direction::Buy, Offset::Open, 3000, 1000)));
    EXPECT_NEAR(pm.getAccount().frozen_margin, 0, kEps);
    EXPECT_FALSE(pm.getOrderFreeze("big").has_value());
}

TEST_F(PositionManagerTest, ExistingReservationsAreNotCountedTwice) {
    // 330 lots ≈ 990,330 needed; a second order must see only what's left
    ASSERT_TRUE(pm.onOrderRequest("a", req("rb2607", Direction::Buy, Offset::Open, 3000, 300)));
    EXPECT_TRUE(pm.onOrderRequest("b", req("rb2607", Direction::Buy, Offset::Open, 3000, 30)));
    EXPECT_FALSE(pm.onOrderRequest("c", req("rb2607", Direction::Buy, Offset::Open, 3000, 30)));
}

TEST_F(PositionManagerTest, ReleaseIsIdempotentAndBounded) {
    ASSERT_TRUE(pm.onOrderRequest("k", req("rb2607", Direction::Buy, Offset::Open, 3000, 1)));
    pm.releaseOrder("k", 5);
    pm.releaseOrder("k", 5);
    auto acc = pm.getAccount();
    EXPECT_NEAR(acc.frozen_margin, 0, kEps);
    EXPECT_NEAR(acc.available, 1'000'000, kEps);
}

TEST_F(PositionManagerTest, ExternalTradeUpdatesPositionWithoutTouchingFreezes) {
    ASSERT_TRUE(pm.onOrderRequest("k", req("rb2607", Direction::Buy, Offset::Open, 3000, 1)));
    pm.onTradeFill(fill("rb2607", Direction::Buy, Offset::Open, 3000, 2));
    EXPECT_EQ(pm.getPosition("rb2607", '2', '1')->position, 2);
    EXPECT_NEAR(pm.getAccount().frozen_margin, 3000, kEps);
}

TEST(PositionManagerNoAccount, SkipsFundsCheckUntilAccountLoaded) {
    PositionManager pm;
    pm.addInstrument(makeInst("rb2607", "rb", "SHFE", '2'));
    EXPECT_TRUE(pm.onOrderRequest("k", req("rb2607", Direction::Buy, Offset::Open, 3000, 1000)));
    EXPECT_FALSE(pm.isAccountLoaded());
}

TEST(PositionManagerNoAccount, UnknownInstrumentIsNotFrozen) {
    PositionManager pm;
    EXPECT_TRUE(pm.onOrderRequest("k", req("xx", Direction::Buy, Offset::Open, 1, 1)));
    EXPECT_FALSE(pm.getOrderFreeze("k").has_value());
}
