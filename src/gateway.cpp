#include "gateway.h"
#include "logger.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

namespace falcon {

FalconGateway::FalconGateway(const GatewayConfig& cfg) : config_(cfg) {
    trader_handler_ = std::make_unique<TraderHandler>();
    md_handler_ = std::make_unique<MdHandler>();
    LOG_INFO("[Gateway] Created. trade_front={} market_front={}", config_.trade_front, config_.market_front);
}

FalconGateway::~FalconGateway() {
    stop();
}

// ============================================================
// Lifecycle
// ============================================================
bool FalconGateway::validateConfig() const {
    // Every field must fit the CTP char[] it is copied into
    CThostFtdcReqUserLoginField l{};
    CThostFtdcReqAuthenticateField a{};
    struct Check { const char* name; bool ok; };
    const Check checks[] = {
        {"broker_id", copyField(l.BrokerID, config_.broker_id)},
        {"user_id",   copyField(l.UserID, config_.user_id)},
        {"password",  copyField(l.Password, config_.password)},
        {"app_id",    copyField(a.AppID, config_.app_id)},
        {"auth_code", copyField(a.AuthCode, config_.auth_code)},
    };
    bool ok = true;
    for (const auto& c : checks) {
        if (!c.ok) {
            LOG_ERROR("[Gateway] Config field {} is too long for CTP", c.name);
            ok = false;
        }
    }
    if (config_.trade_front.empty() || config_.market_front.empty()) {
        LOG_ERROR("[Gateway] trade_front and market_front are required");
        ok = false;
    }
    return ok;
}

bool FalconGateway::start() {
    if (running_) {
        LOG_WARN("[Gateway] Already running");
        return true;
    }
    if (!validateConfig()) return false;

    LOG_INFO("[Gateway] Starting. flow_trade={} flow_market={}",
             config_.flow_path_trade, config_.flow_path_market);

    // Ensure flow directories exist (CTP API will SEGFAULT if missing!)
    for (const auto& path : {config_.flow_path_trade, config_.flow_path_market}) {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) {
                LOG_ERROR("[Gateway] Cannot create flow directory: {} ({})", path, ec.message());
                return false;
            }
        }
    }

    trader_handler_->resetState();
    md_handler_->resetState();
    ready_ = false;
    startup_done_ = false;
    login_fail_reported_ = false;

    // Wire callbacks before Init(): SPI callbacks may fire immediately
    trader_handler_->flow_ctrl = &flow_ctrl_;
    trader_handler_->on_error = [this](int e, const std::string& m) {
        if (error_cb_) error_cb_(e, m);
    };
    trader_handler_->on_instrument = [this](const InstrumentInfo* info, bool last) {
        onInstrumentResponse(info, last);
    };
    trader_handler_->on_commission_rate = [this](const std::string& id, const CommissionInfo& comm) {
        position_mgr_.setCommission(id, comm);
    };
    trader_handler_->on_broker_params = [this](char mpt) {
        position_mgr_.setBrokerMarginPriceType(mpt);
        LOG_INFO("[Gateway] Broker MarginPriceType={}", mpt);
    };
    {
        std::lock_guard lk(sub_mtx_);
        subscribed_.insert(config_.subscribe_list.begin(), config_.subscribe_list.end());
    }

    flow_ctrl_.start();

    trade_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(config_.flow_path_trade.c_str());
    md_api_ = CThostFtdcMdApi::CreateFtdcMdApi(config_.flow_path_market.c_str());
    LOG_INFO("[Gateway] TraderApi version={} MdApi version={}",
             CThostFtdcTraderApi::GetApiVersion(), CThostFtdcMdApi::GetApiVersion());

    running_ = true;
    trade_worker_ = std::thread(&FalconGateway::tradeWorkerLoop, this);
    md_worker_ = std::thread(&FalconGateway::mdWorkerLoop, this);

    trade_api_->RegisterSpi(trader_handler_.get());
    trade_api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    trade_api_->SubscribePublicTopic(THOST_TERT_NONE);
    trade_api_->RegisterFront(const_cast<char*>(config_.trade_front.c_str()));
    trade_api_->Init();

    md_api_->RegisterSpi(md_handler_.get());
    md_api_->RegisterFront(const_cast<char*>(config_.market_front.c_str()));
    md_api_->Init();

    LOG_INFO("[Gateway] Started. Waiting for connection...");
    return true;
}

void FalconGateway::stop() {
    if (!running_.exchange(false)) return;

    LOG_INFO("[Gateway] Stopping...");
    ready_ = false;

    // Workers and the flow thread call into the APIs; stop them first
    trader_handler_->stopping = true;
    if (trade_worker_.joinable()) trade_worker_.join();
    if (md_worker_.joinable()) md_worker_.join();
    flow_ctrl_.stop();

    auto active = order_mgr_.activeOrders();
    if (!active.empty() && trade_api_) {
        LOG_INFO("[Gateway] Cancelling {} active orders...", active.size());
        for (const auto& ord : active) cancelOrderRecord(ord);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (trade_api_) {
        trade_api_->RegisterSpi(nullptr);
        trade_api_->Release();
        trade_api_ = nullptr;
    }
    if (md_api_) {
        md_api_->RegisterSpi(nullptr);
        md_api_->Release();
        md_api_ = nullptr;
    }

    startup_done_ = false;
    LOG_INFO("[Gateway] Stopped");
}

// ============================================================
// Trading
// ============================================================

int FalconGateway::sendOrder(const OrderRequest& req) {
    if (!trade_api_ || !ready_) {
        LOG_ERROR("[Gateway] sendOrder failed: not ready");
        return -1;
    }

    CThostFtdcInputOrderField field{};
    if (req.volume <= 0
        || !copyField(field.InstrumentID, req.instrument) || req.instrument.empty()
        || !copyField(field.ExchangeID, req.exchange)) {
        LOG_ERROR("[Gateway] sendOrder rejected: invalid request inst='{}' exch='{}' vol={}",
                  req.instrument, req.exchange, req.volume);
        return -3;
    }
    (void)copyField(field.BrokerID, config_.broker_id);   // validated in start()
    (void)copyField(field.UserID, config_.user_id);
    (void)copyField(field.InvestorID, config_.user_id);
    field.Direction = static_cast<char>(req.direction);
    field.CombOffsetFlag[0] = static_cast<char>(req.offset);
    field.CombHedgeFlag[0] = req.hedge_flag;
    field.LimitPrice = req.price;
    field.VolumeTotalOriginal = req.volume;

    if (req.type == OrderType::Limit) {
        field.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
        field.TimeCondition = THOST_FTDC_TC_GFD;
        field.VolumeCondition = THOST_FTDC_VC_AV;
    } else {
        field.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
        field.TimeCondition = THOST_FTDC_TC_IOC;
        field.VolumeCondition = THOST_FTDC_VC_AV;
        // 郑商所(CZCE)市价单 LimitPrice 必须为 0, 否则拒单
        if (req.exchange == "CZCE") {
            field.LimitPrice = 0.0;
        }
    }
    field.ContingentCondition = THOST_FTDC_CC_Immediately;
    field.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    field.IsAutoSuspend = 0;
    field.UserForceClose = 0;
    field.MinVolume = 1;

    // Register the order before sending: OnRtnOrder/OnRtnTrade may arrive on
    // the SPI thread before ReqOrderInsert returns.
    std::lock_guard send_lock(send_mtx_);
    int front = trader_handler_->front_id;
    int session = trader_handler_->session_id;
    std::string order_ref = std::to_string(++trader_handler_->max_order_ref);
    if (!copyField(field.OrderRef, order_ref)) return -3;
    auto key = OrderManager::makeKey(front, session, order_ref);

    if (!position_mgr_.onOrderRequest(key, req)) {
        LOG_WARN("[Gateway] sendOrder REJECTED: insufficient funds. inst={} dir={} offset={} px={} vol={}",
                 req.instrument, static_cast<char>(req.direction),
                 static_cast<char>(req.offset), req.price, req.volume);
        if (error_cb_) error_cb_(-100, "Insufficient funds for " + req.instrument);
        return -2;
    }
    if (!order_mgr_.create(req, front, session, order_ref)) {
        LOG_ERROR("[Gateway] Duplicate OrderRef {}", key);
        position_mgr_.releaseOrder(key, req.volume);
        return -4;
    }

    int ret = trade_api_->ReqOrderInsert(&field, req.request_id);
    if (ret != 0) {
        LOG_ERROR("[Gateway] ReqOrderInsert API rejected. inst={} ret={}", req.instrument, ret);
        order_mgr_.remove(key);
        position_mgr_.releaseOrder(key, req.volume);
        return ret;
    }

    LOG_INFO("[Gateway] Order sent. inst={} dir={} offset={} px={:.2f} vol={} ref={}",
             req.instrument, static_cast<char>(req.direction),
             static_cast<char>(req.offset), req.price, req.volume, order_ref);
    return 0;
}

int FalconGateway::cancelOrderRecord(const OrderRecord& ord) {
    if (!trade_api_) return -1;
    CThostFtdcInputOrderActionField field{};
    (void)copyField(field.BrokerID, config_.broker_id);
    (void)copyField(field.UserID, config_.user_id);
    (void)copyField(field.InvestorID, config_.user_id);
    if (!copyField(field.ExchangeID, ord.request.exchange)
        || !copyField(field.InstrumentID, ord.request.instrument)) {
        return -3;
    }
    if (ord.order_sys_id[0] != '\0') {
        (void)copyField(field.OrderSysID, ord.order_sys_id.data());
    } else {
        // Not yet accepted by the exchange: cancel by FrontID + SessionID + OrderRef
        field.FrontID = ord.front_id;
        field.SessionID = ord.session_id;
        (void)copyField(field.OrderRef, ord.order_ref.data());
    }
    field.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = trade_api_->ReqOrderAction(&field, 0);
    if (ret != 0) {
        LOG_ERROR("[Gateway] ReqOrderAction failed. ref={} sys_id={} ret={}",
                  ord.order_ref.data(), ord.order_sys_id.data(), ret);
    }
    return ret;
}

int FalconGateway::cancelOrder(const std::string& order_ref) {
    auto key = OrderManager::makeKey(trader_handler_->front_id, trader_handler_->session_id, order_ref);
    auto ord = order_mgr_.findByKey(key);
    if (!ord) {
        LOG_WARN("[Gateway] cancelOrder: order not found. ref={}", order_ref);
        return -1;
    }
    LOG_INFO("[Gateway] Cancel order. ref={} sys_id={}", order_ref, ord->order_sys_id.data());
    return cancelOrderRecord(*ord);
}

int FalconGateway::cancelOrderBySysID(const std::string& sys_id, const std::string& ex,
                                       const std::string& inst) {
    OrderRecord ord;
    ord.request.exchange = ex;
    ord.request.instrument = inst;
    if (sys_id.empty() || sys_id.size() >= ord.order_sys_id.size()) return -3;
    std::copy(sys_id.begin(), sys_id.end(), ord.order_sys_id.begin());
    return cancelOrderRecord(ord);
}

int FalconGateway::cancelAllActive() {
    auto active = order_mgr_.activeOrders();
    int count = 0;
    for (const auto& ord : active) {
        if (cancelOrderRecord(ord) == 0) count++;
    }
    LOG_INFO("[Gateway] CancelAllActive: {}/{} cancel requests sent", count, active.size());
    return count;
}

// ============================================================
// Market Data
// ============================================================
void FalconGateway::subscribe(const std::vector<std::string>& instruments) {
    if (instruments.empty()) return;
    {
        std::lock_guard lk(sub_mtx_);
        subscribed_.insert(instruments.begin(), instruments.end());
    }
    if (!md_api_) return;

    std::vector<char*> ptrs;
    for (const auto& s : instruments) ptrs.push_back(const_cast<char*>(s.c_str()));
    md_api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    LOG_INFO("[Gateway] Subscribed to {} instruments", ptrs.size());
}

void FalconGateway::unsubscribe(const std::vector<std::string>& instruments) {
    if (instruments.empty()) return;
    {
        std::lock_guard lk(sub_mtx_);
        for (const auto& s : instruments) subscribed_.erase(s);
    }
    if (!md_api_) return;

    std::vector<char*> ptrs;
    for (const auto& s : instruments) ptrs.push_back(const_cast<char*>(s.c_str()));
    md_api_->UnSubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    LOG_INFO("[Gateway] Unsubscribed from {} instruments", ptrs.size());
}

// ============================================================
// Query
// ============================================================
void FalconGateway::submitQuery(const char* name, std::function<int(int)> req, bool verify) {
    flow_ctrl_.submit([this, name, verify, req = std::move(req)](int request_id) {
        if (!trade_api_) return 0;
        trader_handler_->verify_request_id = verify ? request_id : -1;
        LOG_DEBUG("[Gateway] {} request_id={}{}", name, request_id, verify ? " (verify)" : "");
        return req(request_id);
    }, name);
}

void FalconGateway::submitPositionQuery(bool verify, bool replace_snapshot) {
    submitQuery("ReqQryInvestorPosition", [this, replace_snapshot](int id) {
        CThostFtdcQryInvestorPositionField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        // Positions that no longer exist would otherwise survive the snapshot
        if (replace_snapshot) position_mgr_.clearPositions();
        return trade_api_->ReqQryInvestorPosition(&f, id);
    }, verify);
}

void FalconGateway::submitAccountQuery(bool verify) {
    submitQuery("ReqQryTradingAccount", [this](int id) {
        CThostFtdcQryTradingAccountField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        return trade_api_->ReqQryTradingAccount(&f, id);
    }, verify);
}

void FalconGateway::submitOrderQuery(bool verify) {
    submitQuery("ReqQryOrder", [this](int id) {
        CThostFtdcQryOrderField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        return trade_api_->ReqQryOrder(&f, id);
    }, verify);
}

void FalconGateway::submitTradeQuery(bool verify) {
    submitQuery("ReqQryTrade", [this](int id) {
        CThostFtdcQryTradeField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        return trade_api_->ReqQryTrade(&f, id);
    }, verify);
}

void FalconGateway::queryPosition() { submitPositionQuery(false, false); }
void FalconGateway::queryAccount()  { submitAccountQuery(false); }
void FalconGateway::queryOrder()    { submitOrderQuery(false); }
void FalconGateway::queryTrade()    { submitTradeQuery(false); }

void FalconGateway::queryInstrument(const std::string& product_id) {
    submitQuery("ReqQryInstrument", [this, product_id](int id) {
        CThostFtdcQryInstrumentField f{};
        if (!copyField(f.ProductID, product_id)) {
            LOG_ERROR("[Gateway] product_filter '{}' too long", product_id);
            return 0;
        }
        return trade_api_->ReqQryInstrument(&f, id);
    });
}

std::vector<TradeRecord> FalconGateway::tradeHistory() const {
    std::lock_guard lk(history_mtx_);
    return trade_history_;
}

// ============================================================
// Init Sequence
// ============================================================
void FalconGateway::onLoginSuccess() {
    // New trading day → previous day's trades no longer apply
    auto day = tradingDay();
    if (day != history_trading_day_) {
        std::lock_guard lk(history_mtx_);
        trade_history_.clear();
        seen_trades_.clear();
        history_trading_day_ = day;
    }

    LOG_INFO("[Gateway] Loading account, positions, instruments, commission rates...");
    submitQuery("ReqSettlementInfoConfirm", [this](int id) {
        CThostFtdcSettlementInfoConfirmField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        return trade_api_->ReqSettlementInfoConfirm(&f, id);
    });
    submitAccountQuery(false);
    submitPositionQuery(false, true);
    queryInstrument(config_.product_filter);
    submitQuery("ReqQryInstrumentCommissionRate", [this](int id) {
        CThostFtdcQryInstrumentCommissionRateField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        return trade_api_->ReqQryInstrumentCommissionRate(&f, id);
    });
    submitQuery("ReqQryBrokerTradingParams", [this](int id) {
        CThostFtdcQryBrokerTradingParamsField f{};
        (void)copyField(f.BrokerID, config_.broker_id);
        (void)copyField(f.InvestorID, config_.user_id);
        (void)copyField(f.CurrencyID, "CNY");
        return trade_api_->ReqQryBrokerTradingParams(&f, id);
    });
    submitTradeQuery(false);  // trade history
    flow_ctrl_.submitMarker([this]() { startup_done_ = true; });
}

void FalconGateway::reconcileCommission() {
    std::vector<TradeRecord> history = tradeHistory();
    if (history.empty()) return;
    double hist_comm = 0;
    for (const auto& t : history) {
        bool isOpen = (t.offset == Offset::Open);
        hist_comm += position_mgr_.calcCommissionForOrder(t.instrument, t.price, t.volume, isOpen, false);
    }
    position_mgr_.setCommissionBaseline(hist_comm);
    LOG_INFO("[Gateway] Commission baseline from {} trades: {:.2f}", history.size(), hist_comm);
}

void FalconGateway::onReady() {
    reconcileCommission();
    ready_ = true;
    auto acc = position_mgr_.getAccount();
    LOG_INFO("[Gateway] READY. TradingDay={} Instruments={} Balance={:.2f} Available={:.2f}",
             tradingDay(), instrument_count_.load(), acc.balance, acc.available);
    if (connected_cb_) connected_cb_();
}

// ============================================================
// Worker Thread: Trade
// ============================================================
void FalconGateway::tradeWorkerLoop() {
    logger::preallocate();
    LOG_INFO("[TradeWorker] Started");
    TraderEvent ev;
    TradeRecord trd;
    PositionRecord pos;
    AccountRecord acc;

    while (running_) {
        bool work = false;
        // Read before draining: everything pushed before the marker fired is
        // then guaranteed to be processed before READY.
        bool startup_done = startup_done_.load();

        // --- Connection events ---
        if (trader_handler_->front_disconnected.exchange(false)) {
            LOG_WARN("[TradeWorker] Disconnected. Reason=0x{:04X}. Waiting for reconnect...",
                     trader_handler_->disconnect_reason.load());
            ready_ = false;
            startup_done_ = false;
            startup_done = false;
            flow_ctrl_.reset();   // in-flight query is lost with the connection
            work = true;
        }

        if (trader_handler_->front_connected.exchange(false)) {
            LOG_INFO("[TradeWorker] Front connected. Authenticating...");
            CThostFtdcReqAuthenticateField a{};
            (void)copyField(a.BrokerID, config_.broker_id);
            (void)copyField(a.UserID, config_.user_id);
            (void)copyField(a.AppID, config_.app_id);
            (void)copyField(a.AuthCode, config_.auth_code);
            trade_api_->ReqAuthenticate(&a, 0);
            work = true;
        }

        if (trader_handler_->authenticated.exchange(false)) {
            LOG_INFO("[TradeWorker] Authenticated. Logging in as {}...", config_.user_id);
            CThostFtdcReqUserLoginField l{};
            (void)copyField(l.BrokerID, config_.broker_id);
            (void)copyField(l.UserID, config_.user_id);
            (void)copyField(l.Password, config_.password);
            (void)copyField(l.UserProductInfo, "FalconGW");
            trade_api_->ReqUserLogin(&l, 0);
            work = true;
        }

        if (trader_handler_->login_success.exchange(false)) {
            LOG_INFO("[TradeWorker] Login OK. FrontID={} SessionID={} TradingDay={}",
                     trader_handler_->front_id.load(), trader_handler_->session_id.load(),
                     tradingDay());
            login_fail_reported_ = false;
            onLoginSuccess();
            if (login_cb_) login_cb_(true, "Login OK");
            work = true;
        } else if (!trader_handler_->shouldRelogin() && !ready_ && !login_fail_reported_) {
            // Login permanently failed (wrong pw, illegal account, etc.)
            login_fail_reported_ = true;
            LOG_CRITICAL("[TradeWorker] Login FAILED permanently. "
                         "SimNow accounts may require password change via 快期 first.");
            if (login_cb_) login_cb_(false, "Login failed: illegal login or wrong password");
        }

        // --- Order/trade pushes (single queue preserves CTP ordering) ---
        while (trader_handler_->events.tryPop(ev)) {
            if (ev.kind == TraderEvent::Kind::Order) processOrderUpdate(ev.order);
            else processTradeUpdate(ev.trade);
            work = true;
        }

        while (trader_handler_->position_results.tryPop(pos)) {
            position_mgr_.onQueryPosition(pos);
            work = true;
        }

        while (trader_handler_->account_results.tryPop(acc)) {
            position_mgr_.onQueryAccount(acc);
            work = true;
        }

        // Trade history: recorded for de-duplication, already in the snapshots
        while (trader_handler_->trade_query_results.tryPop(trd)) {
            recordTrade(trd);
            work = true;
        }

        if (startup_done && !ready_) onReady();

        if (!work) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    LOG_INFO("[TradeWorker] Stopped");
}

// ============================================================
// Worker Thread: Market Data
// ============================================================
void FalconGateway::mdWorkerLoop() {
    logger::preallocate();
    LOG_INFO("[MdWorker] Started");

    TickData tick;

    while (running_) {
        bool work = false;

        if (md_handler_->front_disconnected.exchange(false)) {
            LOG_WARN("[MdWorker] Disconnected. Waiting for reconnect...");
            work = true;
        }

        if (md_handler_->front_connected.exchange(false)) {
            LOG_INFO("[MdWorker] Front connected. Logging in...");
            CThostFtdcReqUserLoginField l{};
            (void)copyField(l.BrokerID, config_.broker_id);
            (void)copyField(l.UserID, config_.user_id);
            (void)copyField(l.Password, config_.password);
            md_api_->ReqUserLogin(&l, 0);
            work = true;
        }

        if (md_handler_->login_success.exchange(false)) {
            // Restore every subscription (initial login and reconnects)
            std::vector<std::string> subs;
            {
                std::lock_guard lk(sub_mtx_);
                subs.assign(subscribed_.begin(), subscribed_.end());
            }
            if (!subs.empty()) subscribe(subs);
            LOG_INFO("[MdWorker] Market data login OK. Subscribed={}", subs.size());
            work = true;
        }

        while (md_handler_->tick_buffer.tryPop(tick)) {
            processTick(tick);
            work = true;
        }

        if (!work) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    LOG_INFO("[MdWorker] Stopped");
}

// ============================================================
// Event Processing
// ============================================================

void FalconGateway::processOrderUpdate(const OrderRecord& rec) {
    auto result = order_mgr_.update(rec);
    if (!result.found) {
        LOG_DEBUG("[Gateway] External order (not tracked). sys_id={}", rec.order_sys_id.data());
        return;
    }
    if (result.ignored) return;

    const auto& ord = result.record;
    if (result.newly_final) {
        // Fills release their own share in onTradeFill; release the rest here
        int unfilled = ord.request.volume - ord.volume_traded;
        if (ord.status != OrderStatus::AllTraded && unfilled > 0) {
            position_mgr_.releaseOrder(result.key, unfilled);
        }
        LOG_INFO("[Gateway] Order final. inst={} ref={} status={} submit={} filled={}/{}",
                 ord.request.instrument, ord.order_ref.data(), static_cast<char>(ord.status),
                 ord.submit_status, ord.volume_traded, ord.request.volume);
    }
    if (order_cb_) order_cb_(ord);
}

bool FalconGateway::recordTrade(const TradeRecord& rec) {
    std::lock_guard lk(history_mtx_);
    if (!seen_trades_.insert(rec.tradeKey()).second) return false;
    trade_history_.push_back(rec);
    return true;
}

void FalconGateway::processTradeUpdate(const TradeRecord& rec) {
    if (!recordTrade(rec)) {
        LOG_DEBUG("[Gateway] Duplicate trade ignored. trade_id={}", rec.trade_id.data());
        return;
    }

    auto key = order_mgr_.keyByOrderSysID(rec.exchange, rec.order_sys_id.data());
    // Before READY only other sessions' trades (or private-topic replays) can
    // arrive; the position/account snapshots already include them.
    if (!key && !ready_) return;

    position_mgr_.onTradeFill(rec, key.value_or(std::string{}));
    LOG_INFO("[Gateway] Trade fill. inst={} dir={} offset={} px={:.2f} vol={}",
             rec.instrument, static_cast<char>(rec.direction),
             static_cast<char>(rec.offset), rec.price, rec.volume);
    if (trade_cb_) trade_cb_(rec);
}

void FalconGateway::processTick(const TickData& tick) {
    position_mgr_.onTick(tick);
    if (tick_cb_) tick_cb_(tick);
}

// ============================================================
// Instrument loading (SPI thread)
// ============================================================

void FalconGateway::onInstrumentResponse(const InstrumentInfo* info, bool is_last) {
    if (info) {
        position_mgr_.addInstrument(*info);
        int n = ++instruments_loaded_count_;
        if (n % 100 == 0) LOG_DEBUG("[Gateway] Instruments progress: {} loaded...", n);
    }
    if (is_last) {
        instrument_count_ = instruments_loaded_count_.exchange(0);
        LOG_INFO("[Gateway] Instruments loaded: {} contracts", instrument_count_.load());
    }
}

void FalconGateway::verifyLocalState() {
    LOG_INFO("========== ORDER STATE (local) ==========");
    auto active = order_mgr_.activeOrders();
    for (const auto& ord : active) {
        LOG_INFO("[Local] inst={} ref={} sys_id={} status={} filled={}/{} total={}",
                 ord.request.instrument, ord.order_ref.data(), ord.order_sys_id.data(),
                 static_cast<char>(ord.status), ord.volume_traded,
                 ord.request.volume, ord.volume_total);
    }
    if (active.empty()) LOG_INFO("[Local] No active orders.");

    LOG_INFO("========== ACCOUNT STATE (local) ==========");
    auto acc = position_mgr_.getAccount();
    LOG_INFO("[Local] Balance={:.2f} Available={:.2f} CurrMargin={:.2f} "
             "FrozenMargin={:.2f} FrozenComm={:.2f} CloseProfit={:.2f} "
             "PosProfit={:.2f} Commission={:.2f}",
             acc.balance, acc.available, acc.curr_margin,
             acc.frozen_margin, acc.frozen_commission, acc.close_profit,
             acc.position_profit, acc.commission);

    LOG_INFO("========== POSITION STATE (local) ==========");
    std::vector<PositionRecord> local_positions;
    position_mgr_.getAllPositions(local_positions);
    int pos_count = 0;
    for (const auto& p : local_positions) {
        if (p.position > 0 || p.frozen_margin > 0 || p.frozen_commission > 0 || p.long_frozen > 0 || p.short_frozen > 0) {
            pos_count++;
            LOG_INFO("[Local] {} dir={} date={} pos={} today={} yd={} Lfrozen={} Sfrozen={} margin={:.2f} frozen_margin={:.2f} pos_profit={:.2f} close_profit={:.2f}",
                     p.instrument, p.posi_direction, p.position_date,
                     p.position, p.today_position, p.yd_position,
                     p.long_frozen, p.short_frozen,
                     p.margin, p.frozen_margin, p.position_profit, p.close_profit);
        }
    }
    if (pos_count == 0) LOG_INFO("[Local] No active positions.");

    LOG_INFO("========== VERIFICATION COMPLETE ==========");
}

void FalconGateway::compareWithRemote(int timeout_sec) {
    if (!ready_) { LOG_WARN("[Verify] Gateway not ready, skipping comparison"); return; }

    LOG_INFO("========== REMOTE COMPARISON ==========");

    // Drain cmp buffers from any previous (timed-out) run
    { PositionRecord d; while (trader_handler_->cmp_positions.tryPop(d)) {} }
    { AccountRecord d;  while (trader_handler_->cmp_accounts.tryPop(d)) {} }
    { OrderRecord d;    while (trader_handler_->cmp_orders.tryPop(d)) {} }
    { TradeRecord d;    while (trader_handler_->cmp_trades.tryPop(d)) {} }

    // Results are routed by request id, so concurrent normal queries are unaffected
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    submitPositionQuery(true, false);
    submitAccountQuery(true);
    submitOrderQuery(true);
    submitTradeQuery(true);
    flow_ctrl_.submitMarker([done]() { done->set_value(); });

    if (finished.wait_for(std::chrono::seconds(timeout_sec)) != std::future_status::ready) {
        LOG_WARN("[Verify] Remote queries did not complete within {}s, comparing partial data",
                 timeout_sec);
    }

    std::vector<PositionRecord> remote_positions;
    std::vector<AccountRecord>   remote_accounts;
    std::vector<OrderRecord>     remote_orders;
    std::vector<TradeRecord>     remote_trades;
    { PositionRecord r; while (trader_handler_->cmp_positions.tryPop(r)) remote_positions.push_back(r); }
    { AccountRecord r;  while (trader_handler_->cmp_accounts.tryPop(r)) remote_accounts.push_back(r); }
    { OrderRecord r;    while (trader_handler_->cmp_orders.tryPop(r)) remote_orders.push_back(r); }
    { TradeRecord r;    while (trader_handler_->cmp_trades.tryPop(r)) remote_trades.push_back(r); }

    // ---- Compare Account ----
    auto local_acc = position_mgr_.getAccount();
    if (!remote_accounts.empty()) {
        auto& rem = remote_accounts[0];
        LOG_INFO("[Compare] ACCOUNT:");
        LOG_INFO("[Compare]   Local:  Balance={:.2f} Avail={:.2f} Margin={:.2f} FrzMargin={:.2f} FrzComm={:.2f} FrzCash={:.2f} DelMargin={:.2f} Reserve={:.2f}",
                 local_acc.balance, local_acc.available, local_acc.curr_margin,
                 local_acc.frozen_margin, local_acc.frozen_commission,
                 local_acc.frozen_cash, local_acc.delivery_margin, local_acc.reserve);
        LOG_INFO("[Compare]   Remote: Balance={:.2f} Avail={:.2f} Margin={:.2f} FrzMargin={:.2f} FrzComm={:.2f} FrzCash={:.2f} DelMargin={:.2f} Reserve={:.2f}",
                 rem.balance, rem.available, rem.curr_margin,
                 rem.frozen_margin, rem.frozen_commission,
                 rem.frozen_cash, rem.delivery_margin, rem.reserve);
        double bal_diff  = std::abs(local_acc.balance - rem.balance);
        double avl_diff  = std::abs(local_acc.available - rem.available);
        double mgn_diff  = std::abs(local_acc.curr_margin - rem.curr_margin);
        double fmg_diff  = std::abs(local_acc.frozen_margin - rem.frozen_margin);
        bool match = (bal_diff < 0.02 && avl_diff < 0.02 && mgn_diff < 0.02 && fmg_diff < 0.02);
        LOG_INFO("[Compare] ACCOUNT {} (bal:{:.2f} avl:{:.2f} mgn:{:.2f} fmg:{:.2f})",
                 match ? "MATCH" : "MISMATCH", bal_diff, avl_diff, mgn_diff, fmg_diff);
    } else {
        LOG_WARN("[Compare] No remote account data received");
    }

    // ---- Compare Positions ----
    std::vector<PositionRecord> local_positions;
    position_mgr_.getAllPositions(local_positions);
    LOG_INFO("[Compare] POSITION: local={} remote={}", local_positions.size(), remote_positions.size());

    for (const auto& rp : remote_positions) {
        auto lp = position_mgr_.getPosition(rp.instrument, rp.posi_direction, rp.position_date);
        if (lp) {
            bool ok = (lp->position == rp.position && lp->today_position == rp.today_position
                       && std::abs(lp->margin - rp.margin) < 0.02
                       && lp->long_frozen == rp.long_frozen && lp->short_frozen == rp.short_frozen);
            LOG_INFO("[Compare]   {} dir={} dt={} {} pos={}({}) today={}({}) margin={:.2f}({:.2f}) lf={}({}) sf={}({})",
                     rp.instrument, rp.posi_direction, rp.position_date,
                     ok ? "OK" : "!!",
                     lp->position, rp.position,
                     lp->today_position, rp.today_position,
                     lp->margin, rp.margin,
                     lp->long_frozen, rp.long_frozen,
                     lp->short_frozen, rp.short_frozen);
        } else {
            LOG_WARN("[Compare]   {} dir={} dt={} ONLY-REMOTE pos={}",
                     rp.instrument, rp.posi_direction, rp.position_date, rp.position);
        }
    }
    for (const auto& lp : local_positions) {
        if (lp.position <= 0 && lp.frozen_margin <= 0 && lp.long_frozen <= 0 && lp.short_frozen <= 0) continue;
        bool found = false;
        for (const auto& rp : remote_positions) {
            if (rp.instrument == lp.instrument && rp.posi_direction == lp.posi_direction
                && rp.position_date == lp.position_date) { found = true; break; }
        }
        if (!found) {
            LOG_WARN("[Compare]   {} dir={} dt={} ONLY-LOCAL pos={}",
                     lp.instrument, lp.posi_direction, lp.position_date, lp.position);
        }
    }

    // ---- Compare Orders ----
    LOG_INFO("[Compare] ORDER: local-active={} remote={}",
             order_mgr_.activeOrders().size(), remote_orders.size());
    for (const auto& ro : remote_orders) {
        std::string ref(ro.order_ref.data());
        auto lo = order_mgr_.findByKey(OrderManager::makeKey(ro.front_id, ro.session_id, ref));
        if (!lo && ro.order_sys_id[0] != '\0') {
            lo = order_mgr_.findByOrderSysID(ro.request.exchange, ro.order_sys_id.data());
        }
        if (lo) {
            bool ok = (lo->status == ro.status && lo->volume_traded == ro.volume_traded
                       && lo->volume_total == ro.volume_total);
            LOG_INFO("[Compare]   {} ref={} {} status={}({}) filled={}/{}({}/{})",
                     ro.request.instrument, ref, ok ? "OK" : "!!",
                     static_cast<char>(lo->status), static_cast<char>(ro.status),
                     lo->volume_traded, lo->request.volume,
                     ro.volume_traded, ro.volume_total);
        } else {
            LOG_DEBUG("[Compare]   {} ref={} ONLY-REMOTE status={}",
                      ro.request.instrument, ref, static_cast<char>(ro.status));
        }
    }

    // ---- Compare Trades ----
    auto history = tradeHistory();
    std::unordered_set<std::string> local_keys;
    for (const auto& t : history) local_keys.insert(t.tradeKey());
    int t_ok = 0, t_missing = 0;
    for (const auto& rt : remote_trades) {
        if (local_keys.count(rt.tradeKey())) t_ok++;
        else t_missing++;
    }
    LOG_INFO("[Compare] TRADE: local-history={} remote={} matched={} missing-locally={}",
             history.size(), remote_trades.size(), t_ok, t_missing);

    // ---- Commission Reconciliation using trade history ----
    double total_comm = 0;
    for (const auto& t : history) {
        bool isOpen = (t.offset == Offset::Open);
        total_comm += position_mgr_.calcCommissionForOrder(t.instrument, t.price, t.volume, isOpen, false);
    }
    LOG_INFO("[Compare] COMMISSION: local-tracked={:.2f} trade-history-calc={:.2f}",
             position_mgr_.getAccount().commission, total_comm);

    LOG_INFO("========== COMPARISON COMPLETE ==========");
}

} // namespace falcon
