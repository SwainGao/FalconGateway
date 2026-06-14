#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include "gateway.h"
#include "logger.h"
#include "config_loader.h"

std::atomic<bool> g_running{true};
void signalHandler(int) { g_running = false; }

struct TestContext {
    std::string inst;
    std::string exch;
    std::string prod;
    falcon::FalconGateway* gw;
    double upper = 0, lower = 0, last = 0;
    std::atomic<bool> limits_ok{false};
};

void waitForReady(falcon::FalconGateway& gw) {
    while (!gw.isReady() && g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ================================================================
// Single Exchange Test Flow
// ================================================================
bool runExchangeTest(TestContext& ctx) {
    auto& gw = *ctx.gw;
    LOG_INFO("============================================================");
    LOG_INFO("===== TEST: {} ({}) =====", ctx.inst, ctx.exch);

    // --- STEP 0: Baseline ---
    LOG_INFO("[{}] STEP 0: Baseline", ctx.inst);
    gw.compareWithRemote();

    // --- STEP 1: Open position (buy at upper limit → immediate fill) ---
    LOG_INFO("[{}] STEP 1: Open Buy 1@{} (upper limit)", ctx.inst, ctx.upper);
    {
        falcon::OrderRequest req;
        req.instrument = ctx.inst;
        req.exchange   = ctx.exch;
        req.direction  = falcon::Direction::Buy;
        req.offset     = falcon::Offset::Open;
        req.price      = ctx.upper;
        req.volume     = 1;
        req.type       = falcon::OrderType::Limit;
        int ret = gw.sendOrder(req);
        if (ret != 0) { LOG_ERROR("[{}] STEP1 sendOrder failed ret={}", ctx.inst, ret); return false; }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    LOG_INFO("[{}] STEP 1: After open fill", ctx.inst);
    gw.compareWithRemote();

    // --- STEP 2: Close at upper limit (sell too high → no buyer → should NOT fill) ---
    LOG_INFO("[{}] STEP 2: Close Sell 1@{} (upper limit → sell too high, should NOT fill)", ctx.inst, ctx.upper);
    {
        falcon::OrderRequest req;
        req.instrument = ctx.inst;
        req.exchange   = ctx.exch;
        req.direction  = falcon::Direction::Sell;
        req.offset     = falcon::Offset::Close;  // DCE/CZCE/GFEX统一; SHFE区分平今/平昨
        req.price      = ctx.upper;
        req.volume     = 1;
        req.type       = falcon::OrderType::Limit;
        // SHFE: CloseToday for today position
        if (ctx.exch == "SHFE") req.offset = falcon::Offset::CloseToday;
        int ret = gw.sendOrder(req);
        if (ret != 0) { LOG_ERROR("[{}] STEP2 sendOrder failed ret={}", ctx.inst, ret); return false; }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOG_INFO("[{}] STEP 2: After close pending (should be NoTradeQueueing)", ctx.inst);
    gw.compareWithRemote();

    // --- STEP 3: Cancel the pending close ---
    LOG_INFO("[{}] STEP 3: Cancel pending close", ctx.inst);
    gw.cancelAllActive();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOG_INFO("[{}] STEP 3: After cancel", ctx.inst);
    gw.compareWithRemote();

    // --- STEP 4: Close at lower limit (sell cheap → immediate fill) ---
    LOG_INFO("[{}] STEP 4: Close Sell 1@{} (lower limit → sell cheap, should fill)", ctx.inst, ctx.lower);
    {
        falcon::OrderRequest req;
        req.instrument = ctx.inst;
        req.exchange   = ctx.exch;
        req.direction  = falcon::Direction::Sell;
        req.price      = ctx.lower;
        req.volume     = 1;
        req.type       = falcon::OrderType::Limit;
        if (ctx.exch == "SHFE") req.offset = falcon::Offset::CloseToday;
        else req.offset = falcon::Offset::Close;
        int ret = gw.sendOrder(req);
        if (ret != 0) { LOG_ERROR("[{}] STEP4 sendOrder failed ret={}", ctx.inst, ret); return false; }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOG_INFO("[{}] STEP 4: After close fill (position should be flat)", ctx.inst);
    gw.compareWithRemote();

    LOG_INFO("[{}] TEST COMPLETE", ctx.inst);
    return true;
}

int main() {
    logger::init();
    LOG_INFO("[Main] ===== FalconGateway 4-Exchange Full Test =====");
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    falcon::GatewayConfig cfg;
    std::string err_msg;
    if (!falcon::loadConfigFromFile("config.json", cfg, err_msg)) {
        LOG_CRITICAL("[Main] Failed to load config: {}", err_msg);
        return 1;
    }
    LOG_INFO("[Main] Config loaded from config.json");

    // All 4 exchanges
    TestContext tests[] = {
        {"rb2607", "SHFE", "rb", nullptr, 0,0,0,{false}},
        {"TA701",  "CZCE", "TA", nullptr, 0,0,0,{false}},
        {"v2609",  "DCE",  "v",  nullptr, 0,0,0,{false}},
        {"si2607", "GFEX", "si", nullptr, 0,0,0,{false}},
    };
    const int N = 4;

    // Subscribe list MUST be set before gateway construction (config is copied)
    cfg.subscribe_list.clear();
    for (int i = 0; i < N; i++) cfg.subscribe_list.push_back(tests[i].inst);
    cfg.product_filter = "";

    // Register callbacks to capture limits
    falcon::FalconGateway gw(cfg);

    gw.setLoginCallback([](bool ok, const std::string& msg) {
        LOG_INFO("[Main] Login: {} {}", ok ? "OK" : "FAIL", msg);
    });
    gw.setConnectedCallback([]() {});
    gw.setErrorCallback([](int id, const std::string& msg) {
        LOG_ERROR("[Main] Error: {} {}", id, msg);
    });

    gw.setTickCallback([&](const falcon::TickData& tick) {
        for (int i = 0; i < N; i++) {
            if (!tests[i].limits_ok && tick.instrument == tests[i].inst) {
                // Validate limits: must be positive and not CTP invalid sentinel
                if (tick.upper_limit > 0 && tick.upper_limit < 1.0e308 &&
                    tick.lower_limit > 0 && tick.lower_limit < 1.0e308) {
                    tests[i].upper = tick.upper_limit;
                    tests[i].lower = tick.lower_limit;
                    tests[i].last  = tick.last_price;
                    tests[i].limits_ok = true;
                    LOG_INFO("[Main] {} limits: Last={:.2f} Upper={:.2f} Lower={:.2f}",
                             tests[i].inst, tests[i].last, tests[i].upper, tests[i].lower);
                }
            }
        }
    });

    gw.setOrderCallback([&](const falcon::OrderRecord& ord) {
        LOG_INFO("[Order] {} ref={} sys_id={} status={} filled={}/{}",
                 ord.request.instrument, ord.order_ref.data(), ord.order_sys_id.data(),
                 static_cast<char>(ord.status), ord.volume_traded, ord.request.volume);
    });

    gw.setTradeCallback([&](const falcon::TradeRecord& trd) {
        LOG_INFO("[Trade] {} {} px={:.2f} vol={}",
                 trd.instrument, static_cast<char>(trd.direction), trd.price, trd.volume);
    });

    if (!gw.start()) { LOG_CRITICAL("[Main] Failed to start"); return 1; }
    waitForReady(gw);
    if (!gw.isReady()) { LOG_WARN("[Main] Not ready"); return 1; }

    // Wait for all limits captured (30s timeout)
    bool all_ok = false;
    int limit_wait = 0;
    while (!all_ok && g_running && limit_wait < 150) {
        all_ok = true;
        for (int i = 0; i < N; i++) if (!tests[i].limits_ok) all_ok = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        limit_wait++;
    }
    if (!all_ok) {
        for (int i = 0; i < N; i++) {
            if (!tests[i].limits_ok) LOG_ERROR("[Main] {} limits NOT captured", tests[i].inst);
        }
    }

    // Run tests
    int passed = 0;
    for (int i = 0; i < N; i++) {
        tests[i].gw = &gw;
        if (runExchangeTest(tests[i])) passed++;
    }

    LOG_INFO("===== ALL TESTS DONE: {}/{} passed =====", passed, N);

    while (g_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    gw.stop();
    LOG_INFO("[Main] Done.");
    return 0;
}
