#pragma once
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include "ctp/ThostFtdcTraderApi.h"
#include "ctp/ThostFtdcMdApi.h"
#include "trader_handler.h"
#include "md_handler.h"
#include "order_manager.h"
#include "position_manager.h"
#include "flow_controller.h"
#include "types.h"

namespace falcon {

class FalconGateway {
public:
    explicit FalconGateway(const GatewayConfig& cfg);
    ~FalconGateway();

    // Non-copyable
    FalconGateway(const FalconGateway&) = delete;
    FalconGateway& operator=(const FalconGateway&) = delete;

    // ===== Lifecycle =====
    bool start();    // Init + connect + login
    void stop();     // Cancel all orders + release

    // ===== Trading =====
    int sendOrder(const OrderRequest& req);
    int cancelOrder(const std::string& order_ref);
    int cancelOrderBySysID(const std::string& order_sys_id, const std::string& exchange,
                           const std::string& instrument);
    int cancelAllActive();  // Cancel all pending orders

    // ===== Market Data =====
    void subscribe(const std::vector<std::string>& instruments);
    void unsubscribe(const std::vector<std::string>& instruments);

    // ===== Query (async, results via callbacks) =====
    void queryPosition();
    void queryAccount();
    void queryInstrument(const std::string& product_id = "");
    void queryOrder();
    void queryTrade();

    // ===== Comparison / Verification =====
    void verifyLocalState();   // Dump local state only
    void compareWithRemote();  // Query CTP and compare local vs remote, log mismatches

    // ===== Callback Registration =====
    void setTickCallback(TickCallback cb)    { tick_cb_ = std::move(cb); }
    void setOrderCallback(OrderCallback cb)  { order_cb_ = std::move(cb); }
    void setTradeCallback(TradeCallback cb)  { trade_cb_ = std::move(cb); }
    void setLoginCallback(LoginCallback cb)  { login_cb_ = std::move(cb); }
    void setErrorCallback(ErrorCallback cb)  { error_cb_ = std::move(cb); }
    void setConnectedCallback(ConnectedCallback cb) { connected_cb_ = std::move(cb); }

    // ===== Instrument loading =====
    void onInstrumentResponse(CThostFtdcInstrumentField* pInst, bool bIsLast);

    // ===== State =====
    bool isReady() const { return ready_; }
    int frontID() const { return trader_handler_->front_id; }
    int sessionID() const { return trader_handler_->session_id; }
    std::string tradingDay() const { return trader_handler_->trading_day; }

private:
    GatewayConfig config_;

    // CTP API instances
    CThostFtdcTraderApi* trade_api_ = nullptr;
    CThostFtdcMdApi* md_api_ = nullptr;

    // Handlers
    std::unique_ptr<TraderHandler> trader_handler_;
    std::unique_ptr<MdHandler> md_handler_;

    // Managers (accessed from worker thread only)
    OrderManager order_mgr_;
    PositionManager position_mgr_;
    FlowController flow_ctrl_;

    // Worker threads
    std::unique_ptr<std::thread> trade_worker_;
    std::unique_ptr<std::thread> md_worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};

    // Init state tracking
    std::atomic<bool> settlement_confirmed_{false};
    std::atomic<bool> positions_loaded_{false};
    std::atomic<bool> account_loaded_{false};
    std::atomic<bool> instruments_loaded_{false};
    int instrument_count_ = 0;
    int instruments_loaded_count_ = 0;

    // Remote query result storage (for comparison/verification)
    std::vector<OrderRecord> remote_orders_;
    std::vector<TradeRecord> remote_trades_;

    // Local trade history (persistent across session, loaded from CTP at startup)
    std::vector<TradeRecord> trade_history_;

    // Callbacks
    TickCallback tick_cb_;
    OrderCallback order_cb_;
    TradeCallback trade_cb_;
    LoginCallback login_cb_;
    ErrorCallback error_cb_;
    ConnectedCallback connected_cb_;

    // ===== Internal Methods =====
    void tradeWorkerLoop();
    void mdWorkerLoop();

    // Process ring buffer events
    void processOrderUpdate(const OrderRecord& rec);
    void processTradeUpdate(const TradeRecord& rec);
    void processPositionResult(const PositionRecord& rec);
    void processAccountResult(const AccountRecord& rec);
    void processTick(const TickData& tick);

    // Init sequence (chained callbacks via flow controller)
    void onLoginSuccess();
    void confirmSettlement();
    void startQueries();
    void checkReady();
};

} // namespace falcon
