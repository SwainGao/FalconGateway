#pragma once
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#include "ctp_api.h"
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
    // Returns 0 on success, -1 not ready, -2 insufficient funds, -3 invalid
    // request, -4 duplicate OrderRef, or the ReqOrderInsert error code.
    int sendOrder(const OrderRequest& req);
    int cancelOrder(const std::string& order_ref);
    int cancelOrderBySysID(const std::string& order_sys_id, const std::string& exchange,
                           const std::string& instrument);
    int cancelAllActive();  // Cancel all pending orders

    // ===== Market Data =====
    // Subscriptions are remembered and restored after a market-data reconnect.
    void subscribe(const std::vector<std::string>& instruments);
    void unsubscribe(const std::vector<std::string>& instruments);

    // ===== Query (async, results update local state) =====
    void queryPosition();
    void queryAccount();
    void queryInstrument(const std::string& product_id = "");
    void queryOrder();
    void queryTrade();

    // ===== Comparison / Verification =====
    void verifyLocalState();   // Dump local state only
    // Query CTP and compare local vs remote, log mismatches. Blocks until the
    // queries complete (or timeout_sec elapses).
    void compareWithRemote(int timeout_sec = 30);

    // ===== Callback Registration (set before start()) =====
    void setTickCallback(TickCallback cb)    { tick_cb_ = std::move(cb); }
    void setOrderCallback(OrderCallback cb)  { order_cb_ = std::move(cb); }
    void setTradeCallback(TradeCallback cb)  { trade_cb_ = std::move(cb); }
    void setLoginCallback(LoginCallback cb)  { login_cb_ = std::move(cb); }
    void setErrorCallback(ErrorCallback cb)  { error_cb_ = std::move(cb); }
    void setConnectedCallback(ConnectedCallback cb) { connected_cb_ = std::move(cb); }

    // ===== State =====
    bool isReady() const { return ready_; }
    int frontID() const { return trader_handler_->front_id; }
    int sessionID() const { return trader_handler_->session_id; }
    std::string tradingDay() const { return trader_handler_->tradingDay(); }
    AccountRecord getAccount() const { return position_mgr_.getAccount(); }
    std::vector<OrderRecord> activeOrders() const { return order_mgr_.activeOrders(); }
    std::vector<TradeRecord> tradeHistory() const;

private:
    GatewayConfig config_;

    // CTP API instances
    CThostFtdcTraderApi* trade_api_ = nullptr;
    CThostFtdcMdApi* md_api_ = nullptr;

    // Handlers
    std::unique_ptr<TraderHandler> trader_handler_;
    std::unique_ptr<MdHandler> md_handler_;

    OrderManager order_mgr_;
    PositionManager position_mgr_;
    FlowController flow_ctrl_;

    // Worker threads
    std::thread trade_worker_;
    std::thread md_worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};

    // Set by a FlowController marker once all startup queries have completed
    std::atomic<bool> startup_done_{false};
    std::atomic<int> instruments_loaded_count_{0};
    std::atomic<int> instrument_count_{0};
    bool login_fail_reported_ = false;   // trade worker only
    std::string history_trading_day_;    // trade worker only

    // Serializes OrderRef allocation + ReqOrderInsert (CTP needs increasing refs)
    std::mutex send_mtx_;

    // Trade history of the current trading day, de-duplicated by trade key
    mutable std::mutex history_mtx_;
    std::vector<TradeRecord> trade_history_;
    std::unordered_set<std::string> seen_trades_;

    std::mutex sub_mtx_;
    std::set<std::string> subscribed_;

    // Callbacks
    TickCallback tick_cb_;
    OrderCallback order_cb_;
    TradeCallback trade_cb_;
    LoginCallback login_cb_;
    ErrorCallback error_cb_;
    ConnectedCallback connected_cb_;

    // ===== Internal Methods =====
    bool validateConfig() const;
    void tradeWorkerLoop();
    void mdWorkerLoop();

    void processOrderUpdate(const OrderRecord& rec);
    void processTradeUpdate(const TradeRecord& rec);
    bool recordTrade(const TradeRecord& rec);
    void processTick(const TickData& tick);
    void onInstrumentResponse(const InstrumentInfo* info, bool is_last);
    int cancelOrderRecord(const OrderRecord& ord);

    // Queries go through the flow controller; verify=true routes the
    // responses to the comparison buffers.
    void submitQuery(const char* name, std::function<int(int)> req, bool verify = false);
    void submitPositionQuery(bool verify, bool replace_snapshot);
    void submitAccountQuery(bool verify);
    void submitOrderQuery(bool verify);
    void submitTradeQuery(bool verify);

    void onLoginSuccess();
    void onReady();
    void reconcileCommission();
};

} // namespace falcon
