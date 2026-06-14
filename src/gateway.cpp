#include "gateway.h"
#include "logger.h"
#include <cstring>
#include <chrono>
#include <thread>
#include <filesystem>

namespace falcon {

FalconGateway::FalconGateway(const GatewayConfig& cfg) : config_(cfg) {
    trader_handler_ = std::make_unique<TraderHandler>();
    md_handler_ = std::make_unique<MdHandler>();
    LOG_INFO("[Gateway] Created. trade_front={} market_front={}", config_.trade_front, config_.market_front);
}

FalconGateway::~FalconGateway() {
    LOG_INFO("[Gateway] Destroying...");
    stop();
}

// ============================================================
// Lifecycle
// ============================================================
bool FalconGateway::start() {
    if (running_) {
        LOG_WARN("[Gateway] Already running");
        return true;
    }

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

    trade_api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(config_.flow_path_trade.c_str());
    LOG_INFO("[Gateway] TraderApi created. version={}", trade_api_->GetApiVersion());

    trade_api_->RegisterSpi(trader_handler_.get());
    trade_api_->SubscribePrivateTopic(THOST_TERT_RESTART);
    trade_api_->SubscribePublicTopic(THOST_TERT_NONE);
    trade_api_->RegisterFront(const_cast<char*>(config_.trade_front.c_str()));
    trade_api_->Init();

    md_api_ = CThostFtdcMdApi::CreateFtdcMdApi(config_.flow_path_market.c_str());
    LOG_INFO("[Gateway] MdApi created. version={}", md_api_->GetApiVersion());

    md_api_->RegisterSpi(md_handler_.get());
    md_api_->RegisterFront(const_cast<char*>(config_.market_front.c_str()));
    md_api_->Init();

    trader_handler_->order_mgr = &order_mgr_;
    trader_handler_->position_mgr = &position_mgr_;
    trader_handler_->flow_ctrl = &flow_ctrl_;
    trader_handler_->on_error = [this](int e, const std::string& m) {
        LOG_ERROR("[TraderSPI] err={} msg={}", e, m);
        if (error_cb_) error_cb_(e, m);
    };
    trader_handler_->on_instrument = [this](CThostFtdcInstrumentField* inst, bool last) {
        onInstrumentResponse(inst, last);
    };
    trader_handler_->on_commission_rate = [this](const std::string& id,
        double om, double ov, double cm, double cv, double ctm, double ctv) {
        CommissionInfo comm;
        comm.open_ratio_by_money = om;
        comm.open_ratio_by_volume = ov;
        comm.close_ratio_by_money = cm;
        comm.close_ratio_by_volume = cv;
        comm.close_today_ratio_by_money = ctm;
        comm.close_today_ratio_by_volume = ctv;
        position_mgr_.setCommission(id, comm);
    };
    trader_handler_->on_broker_params = [this](char mpt) {
        position_mgr_.setBrokerMarginPriceType(mpt);
        LOG_INFO("[Gateway] Broker MarginPriceType={}", mpt);
    };

    running_ = true;
    trade_worker_ = std::make_unique<std::thread>(&FalconGateway::tradeWorkerLoop, this);
    md_worker_ = std::make_unique<std::thread>(&FalconGateway::mdWorkerLoop, this);

    LOG_INFO("[Gateway] Started. Waiting for connection...");
    return true;
}

void FalconGateway::stop() {
    if (!running_) return;

    LOG_INFO("[Gateway] Stopping...");

    running_ = false;

    int active = 0;
    order_mgr_.forEachActive([&](const OrderRecord& ord) {
        active++;
        if (ord.order_sys_id[0] != '\0') {
            cancelOrderBySysID(std::string(ord.order_sys_id.data()),
                               ord.request.exchange, ord.request.instrument);
        }
    });

    if (active > 0) {
        LOG_INFO("[Gateway] Cancelling {} active orders...", active);
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

    if (trade_worker_ && trade_worker_->joinable()) trade_worker_->join();
    if (md_worker_ && md_worker_->joinable()) md_worker_->join();

    ready_ = false;
    LOG_INFO("[Gateway] Stopped");
}

// ============================================================
// Trading
// ============================================================

int FalconGateway::sendOrder(const OrderRequest& req) {
    if (!trade_api_) {
        LOG_ERROR("[Gateway] sendOrder failed: API not initialized");
        return -1;
    }
    if (!ready_) {
        LOG_ERROR("[Gateway] sendOrder failed: not ready");
        return -1;
    }

    // ---- Pre-trade risk check via PositionManager ----
    bool ok = position_mgr_.onOrderRequest(req);
    if (!ok) {
        LOG_WARN("[Gateway] sendOrder REJECTED: insufficient margin. inst={} dir={} offset={} px={} vol={}",
                 req.instrument, static_cast<char>(req.direction),
                 static_cast<char>(req.offset), req.price, req.volume);
        if (error_cb_) error_cb_(-100, "Insufficient margin for " + req.instrument);
        return -2;
    }

    CThostFtdcInputOrderField field{};
    strcpy(field.BrokerID, config_.broker_id.c_str());
    strcpy(field.UserID, config_.user_id.c_str());
    strcpy(field.InvestorID, config_.user_id.c_str());
    strcpy(field.InstrumentID, req.instrument.c_str());
    strcpy(field.ExchangeID, req.exchange.c_str());
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

    std::string order_ref = std::to_string(++trader_handler_->max_order_ref);
    strcpy(field.OrderRef, order_ref.c_str());

    int ret = trade_api_->ReqOrderInsert(&field, req.request_id);
    if (ret != 0) {
        LOG_ERROR("[Gateway] ReqOrderInsert API rejected. inst={} ret={}", req.instrument, ret);
        position_mgr_.onOrderRejectOrCancel(req, req.volume);
        return ret;
    }

    order_mgr_.create(req, trader_handler_->front_id, trader_handler_->session_id, order_ref);
    LOG_INFO("[Gateway] Order sent. inst={} dir={} offset={} px={:.2f} vol={} ref={}",
             req.instrument, static_cast<char>(req.direction),
             static_cast<char>(req.offset), req.price, req.volume, order_ref);
    return ret;
}

int FalconGateway::cancelOrder(const std::string& order_ref) {
    auto key = std::to_string(trader_handler_->front_id) + "_"
             + std::to_string(trader_handler_->session_id) + "_" + order_ref;
    auto* ord = order_mgr_.findByOrderRef(key);
    if (!ord) {
        LOG_WARN("[Gateway] cancelOrder: order not found. ref={}", order_ref);
        return -1;
    }
    LOG_INFO("[Gateway] Cancel order. ref={} sys_id={}", order_ref, ord->order_sys_id.data());
    return cancelOrderBySysID(std::string(ord->order_sys_id.data()),
                              ord->request.exchange, ord->request.instrument);
}

int FalconGateway::cancelOrderBySysID(const std::string& sys_id, const std::string& ex,
                                       const std::string& inst) {
    if (!trade_api_) {
        LOG_ERROR("[Gateway] cancelOrderBySysID: API not initialized");
        return -1;
    }
    CThostFtdcInputOrderActionField field{};
    strcpy(field.BrokerID, config_.broker_id.c_str());
    strcpy(field.UserID, config_.user_id.c_str());
    strcpy(field.InvestorID, config_.user_id.c_str());
    strcpy(field.OrderSysID, sys_id.c_str());
    strcpy(field.ExchangeID, ex.c_str());
    strcpy(field.InstrumentID, inst.c_str());
    field.ActionFlag = THOST_FTDC_AF_Delete;

    int ret = trade_api_->ReqOrderAction(&field, 0);
    if (ret != 0) {
        LOG_ERROR("[Gateway] ReqOrderAction failed. sys_id={} ret={}", sys_id, ret);
    }
    return ret;
}

// ============================================================
// Market Data
// ============================================================
void FalconGateway::subscribe(const std::vector<std::string>& instruments) {
    if (!md_api_ || instruments.empty()) return;

    std::vector<char*> ptrs;
    for (const auto& s : instruments) ptrs.push_back(const_cast<char*>(s.c_str()));

    int count = static_cast<int>(ptrs.size());
    md_api_->SubscribeMarketData(ptrs.data(), count);
    LOG_INFO("[Gateway] Subscribed to {} instruments", count);
}

void FalconGateway::unsubscribe(const std::vector<std::string>& instruments) {
    if (!md_api_ || instruments.empty()) return;

    std::vector<char*> ptrs;
    for (const auto& s : instruments) ptrs.push_back(const_cast<char*>(s.c_str()));

    int count = static_cast<int>(ptrs.size());
    md_api_->UnSubscribeMarketData(ptrs.data(), count);
    LOG_INFO("[Gateway] Unsubscribed from {} instruments", count);
}

// ============================================================
// Query
// ============================================================
void FalconGateway::queryPosition() {
    flow_ctrl_.submit([this]() {
        CThostFtdcQryInvestorPositionField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryInvestorPosition");
        trade_api_->ReqQryInvestorPosition(&f, 0);
    });
}

void FalconGateway::queryAccount() {
    flow_ctrl_.submit([this]() {
        CThostFtdcQryTradingAccountField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryTradingAccount");
        trade_api_->ReqQryTradingAccount(&f, 0);
    });
}

void FalconGateway::queryOrder() {
    flow_ctrl_.submit([this]() {
        CThostFtdcQryOrderField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryOrder");
        trade_api_->ReqQryOrder(&f, 0);
    });
}

void FalconGateway::queryTrade() {
    flow_ctrl_.submit([this]() {
        CThostFtdcQryTradeField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryTrade");
        trade_api_->ReqQryTrade(&f, 0);
    });
}

void FalconGateway::queryInstrument(const std::string& product_id) {
    flow_ctrl_.submit([this, product_id]() {
        CThostFtdcQryInstrumentField f{};
        if (!product_id.empty()) {
            strcpy(f.ProductID, product_id.c_str());
            LOG_DEBUG("[Gateway] ReqQryInstrument product={}", product_id);
        } else {
            LOG_DEBUG("[Gateway] ReqQryInstrument (all)");
        }
        trade_api_->ReqQryInstrument(&f, 0);
    });
}

// ============================================================
// Init Sequence
// ============================================================
void FalconGateway::onLoginSuccess() {
    confirmSettlement();
}

void FalconGateway::confirmSettlement() {
    flow_ctrl_.submit([this]() {
        CThostFtdcSettlementInfoConfirmField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqSettlementInfoConfirm");
        trade_api_->ReqSettlementInfoConfirm(&f, 0);
    });

    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        startQueries();
    }).detach();
}

void FalconGateway::startQueries() {
    LOG_INFO("[Gateway] Loading account, positions, instruments, commission rates...");
    queryAccount();
    queryPosition();
    queryInstrument(config_.product_filter);
    // Commission rates chained after instrument loading
    flow_ctrl_.submit([this]() {
        CThostFtdcQryInstrumentCommissionRateField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryInstrumentCommissionRate (all positions)");
        trade_api_->ReqQryInstrumentCommissionRate(&f, 0);
    });
    // Broker trading params chained next (MarginPriceType)
    flow_ctrl_.submit([this]() {
        CThostFtdcQryBrokerTradingParamsField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        strcpy(f.CurrencyID, "CNY");
        LOG_DEBUG("[Gateway] ReqQryBrokerTradingParams");
        trade_api_->ReqQryBrokerTradingParams(&f, 0);
    });
    // Trade history chained next
    flow_ctrl_.submit([this]() {
        CThostFtdcQryTradeField f{};
        strcpy(f.BrokerID, config_.broker_id.c_str());
        strcpy(f.InvestorID, config_.user_id.c_str());
        LOG_DEBUG("[Gateway] ReqQryTrade (history)");
        trade_api_->ReqQryTrade(&f, 0);
    });

    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        positions_loaded_ = true;
        account_loaded_ = true;
    }).detach();
}

void FalconGateway::checkReady() {
    if (!ready_ && positions_loaded_ && account_loaded_) {
        ready_ = true;
        auto acc = position_mgr_.getAccount();
        LOG_INFO("[Gateway] READY. TradingDay={} Instruments={} Balance={:.2f} Available={:.2f}",
                 tradingDay(), instrument_count_, acc.balance, acc.available);
        if (connected_cb_) connected_cb_();
    }
}

// ============================================================
// Worker Thread: Trade
// ============================================================
void FalconGateway::tradeWorkerLoop() {
    logger::preallocate();
    LOG_INFO("[TradeWorker] Started");
    OrderRecord ord;
    TradeRecord trd;
    PositionRecord pos;
    AccountRecord acc;

    while (running_) {
        bool work = false;

        // --- Connection events ---
        if (trader_handler_->front_connected.exchange(false)) {
            LOG_INFO("[TradeWorker] Front connected. Authenticating...");
            CThostFtdcReqAuthenticateField a{};
            strcpy(a.BrokerID, config_.broker_id.c_str());
            strcpy(a.UserID, config_.user_id.c_str());
            strcpy(a.AppID, config_.app_id.c_str());
            strcpy(a.AuthCode, config_.auth_code.c_str());
            trade_api_->ReqAuthenticate(&a, 0);
            work = true;
        }

        if (trader_handler_->authenticated.exchange(false)) {
            LOG_INFO("[TradeWorker] Authenticated. Logging in as {}...", config_.user_id);
            CThostFtdcReqUserLoginField l{};
            strcpy(l.BrokerID, config_.broker_id.c_str());
            strcpy(l.UserID, config_.user_id.c_str());
            strcpy(l.Password, config_.password.c_str());
            snprintf(l.UserProductInfo, sizeof(l.UserProductInfo), "%s", "FalconGW");
            trade_api_->ReqUserLogin(&l, 0);
            work = true;
        }

        if (trader_handler_->login_success.exchange(false)) {
            LOG_INFO("[TradeWorker] Login OK. FrontID={} SessionID={} TradingDay={}",
                     trader_handler_->front_id, trader_handler_->session_id,
                     trader_handler_->trading_day);
            onLoginSuccess();
            if (login_cb_) login_cb_(true, "Login OK");
            work = true;
        } else if (!trader_handler_->shouldRelogin() && !ready_) {
            // Login permanently failed (wrong pw, illegal account, etc.)
            static bool login_fail_reported = false;
            if (!login_fail_reported) {
                login_fail_reported = true;
                LOG_CRITICAL("[TradeWorker] Login FAILED permanently. "
                             "SimNow accounts may require password change via 快期 first.");
                if (login_cb_) login_cb_(false, "Login failed: illegal login or wrong password");
            }
        }

        if (trader_handler_->front_disconnected.exchange(false)) {
            LOG_WARN("[TradeWorker] Disconnected. Reason=0x{:04X}",
                     trader_handler_->disconnect_reason.load());
            ready_ = false;
        }

        // --- Order updates ---
        while (trader_handler_->order_updates.tryPop(ord)) {
            processOrderUpdate(ord);
            work = true;
        }

        // --- Trade updates ---
        while (trader_handler_->trade_updates.tryPop(trd)) {
            processTradeUpdate(trd);
            work = true;
        }

        // --- Position query results ---
        while (trader_handler_->position_results.tryPop(pos)) {
            position_mgr_.onQueryPosition(pos);
            work = true;
        }

        // --- Account query results ---
        while (trader_handler_->account_results.tryPop(acc)) {
            position_mgr_.onQueryAccount(acc);
            work = true;
        }


        // --- Order query results (for comparison/verification) ---
        while (trader_handler_->order_query_results.tryPop(ord)) {
            remote_orders_.push_back(ord);
            work = true;
        }

        // --- Trade query results ---
        while (trader_handler_->trade_query_results.tryPop(trd)) {
            remote_trades_.push_back(trd);
            // De-duplicate into trade_history_
            bool dup = false;
            for (const auto& t : trade_history_) {
                if (std::string(t.trade_id.data()) == std::string(trd.trade_id.data())) { dup = true; break; }
            }
            if (!dup) trade_history_.push_back(trd);
            work = true;
        }

        // Reconcile commission from loaded trade history
        if (trade_history_.size() > 0 && account_loaded_) {
            static bool reconciled = false;
            if (!reconciled) {
                double hist_comm = 0;
                for (const auto& t : trade_history_) {
                    bool isOpen = (t.offset == Offset::Open);
                    hist_comm += position_mgr_.calcCommissionForOrder(
                        t.instrument, t.price, t.volume, isOpen, false);
                }
                position_mgr_.setCommissionBaseline(hist_comm);
                LOG_INFO("[Gateway] Commission baseline from {} trades: {:.2f}",
                         trade_history_.size(), hist_comm);
                reconciled = true;
            }
        }

        checkReady();

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
        if (md_handler_->front_connected.exchange(false)) {
            LOG_INFO("[MdWorker] Front connected. Logging in...");
            CThostFtdcReqUserLoginField l{};
            strcpy(l.BrokerID, config_.broker_id.c_str());
            strcpy(l.UserID, config_.user_id.c_str());
            strcpy(l.Password, config_.password.c_str());
            md_api_->ReqUserLogin(&l, 0);
        }

        if (md_handler_->login_success.exchange(false)) {
            if (!config_.subscribe_list.empty()) {
                subscribe(config_.subscribe_list);
            }
            LOG_INFO("[MdWorker] Market data login OK. Subscribed={}", config_.subscribe_list.size());
        }

        while (md_handler_->tick_buffer.tryPop(tick)) {
            processTick(tick);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    LOG_INFO("[MdWorker] Stopped");
}

// ============================================================
// Event Processing
// ============================================================

void FalconGateway::processOrderUpdate(const OrderRecord& rec) {
    auto result = order_mgr_.update(
        rec.front_id, rec.session_id,
        std::string(rec.order_ref.data()),
        std::string(rec.order_sys_id.data()),
        static_cast<char>(rec.status), rec.submit_status,
        rec.volume_traded, rec.volume_total, "");

    if (result.record && result.newly_final) {
        if (result.is_cancel || result.is_exchange_reject) {
            int unfilled = rec.volume_total;
            if (unfilled == 0) unfilled = rec.request.volume - rec.volume_traded;
            if (unfilled > 0) {
                LOG_DEBUG("[Gateway] Order cancelled. inst={} ref={} sys_id={} unfilled={}",
                          rec.request.instrument, rec.order_ref.data(),
                          rec.order_sys_id.data(), unfilled);
                position_mgr_.onOrderRejectOrCancel(rec.request, unfilled);
            }
        }
    }

    if (result.record) {
        if (result.newly_final) {
            LOG_INFO("[Gateway] Order final. inst={} ref={} status={} filled={}/{}",
                     rec.request.instrument, rec.order_ref.data(),
                     static_cast<char>(rec.status), rec.volume_traded, rec.request.volume);
        }
        if (order_cb_) order_cb_(*result.record);
    } else {
        LOG_DEBUG("[Gateway] External order (not tracked). sys_id={}", rec.order_sys_id.data());
    }
}

void FalconGateway::processTradeUpdate(const TradeRecord& rec) {
    position_mgr_.onTradeFill(rec);
    // Add to local trade history
    trade_history_.push_back(rec);
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
// Instrument loading
// ============================================================

void FalconGateway::onInstrumentResponse(CThostFtdcInstrumentField* pInst, bool bIsLast) {
    if (!pInst) {
        if (bIsLast) {
            instruments_loaded_ = true;
            instrument_count_ = static_cast<int>(instruments_loaded_count_);
            LOG_INFO("[Gateway] Instruments loaded: {} contracts", instrument_count_);
            checkReady();
        }
        return;
    }

    InstrumentInfo info;
    info.product_id = pInst->InstrumentID;
    info.exchange_id = pInst->ExchangeID;
    info.product_class = pInst->ProductClass;
    info.volume_multiple = pInst->VolumeMultiple;
    info.price_tick = pInst->PriceTick;
    info.long_margin_ratio = pInst->LongMarginRatio;
    info.short_margin_ratio = pInst->ShortMarginRatio;

    if (info.long_margin_ratio > 1.0) info.long_margin_ratio /= 100.0;
    if (info.short_margin_ratio > 1.0) info.short_margin_ratio /= 100.0;

    info.position_date_type = pInst->PositionDateType;
    info.max_margin_side = pInst->MaxMarginSideAlgorithm;
    info.max_limit_order_volume = pInst->MaxLimitOrderVolume;
    info.min_limit_order_volume = pInst->MinLimitOrderVolume;
    info.max_market_order_volume = pInst->MaxMarketOrderVolume;
    info.min_market_order_volume = pInst->MinMarketOrderVolume;
    info.is_trading = pInst->IsTrading;
    info.inst_life_phase = pInst->InstLifePhase;

    if (info.isOption()) {
        info.strike_price = pInst->StrikePrice;
        info.options_type = pInst->OptionsType;
        info.underlying_id = pInst->UnderlyingInstrID;
        info.underlying_multiple = pInst->UnderlyingMultiple;
    }

    position_mgr_.addInstrument(info);
    instruments_loaded_count_++;

    if (instruments_loaded_count_ % 100 == 0) {
        LOG_DEBUG("[Gateway] Instruments progress: {} loaded...", instruments_loaded_count_);
    }
}

int FalconGateway::cancelAllActive() {
    int count = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> to_cancel;
    order_mgr_.forEachActive([&](const OrderRecord& ord) {
        std::string sys_id(ord.order_sys_id.data());
        if (!sys_id.empty() && sys_id[0] != '\0') {
            to_cancel.push_back({sys_id, ord.request.exchange, ord.request.instrument});
        }
    });
    for (const auto& [sys_id, ex, inst] : to_cancel) {
        int ret = cancelOrderBySysID(sys_id, ex, inst);
        if (ret == 0) count++;
    }
    LOG_INFO("[Gateway] CancelAllActive: {}/{} orders cancelled", count, to_cancel.size());
    return count;
}

void FalconGateway::verifyLocalState() {
    // ---- Dump local order state ----
    LOG_INFO("========== ORDER STATE (local) ==========");
    int active_count = 0;
    order_mgr_.forEachActive([&](const OrderRecord& ord) {
        active_count++;
        LOG_INFO("[Local] inst={} ref={} sys_id={} status={} filled={}/{} total={}",
                 ord.request.instrument, ord.order_ref.data(), ord.order_sys_id.data(),
                 static_cast<char>(ord.status), ord.volume_traded,
                 ord.request.volume, ord.volume_total);
    });
    if (active_count == 0) LOG_INFO("[Local] No active orders.");

    // ---- Dump local account state ----
    LOG_INFO("========== ACCOUNT STATE (local) ==========");
    auto acc = position_mgr_.getAccount();
    LOG_INFO("[Local] Balance={:.2f} Available={:.2f} CurrMargin={:.2f} "
             "FrozenMargin={:.2f} FrozenComm={:.2f} CloseProfit={:.2f} "
             "PosProfit={:.2f} Commission={:.2f}",
             acc.balance, acc.available, acc.curr_margin,
             acc.frozen_margin, acc.frozen_commission, acc.close_profit,
             acc.position_profit, acc.commission);

    // ---- Dump local position state ----
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

void FalconGateway::compareWithRemote() {
    if (!ready_) { LOG_WARN("[Verify] Gateway not ready, skipping comparison"); return; }

    LOG_INFO("========== REMOTE COMPARISON ==========");

    // Switch to verification mode (redirects SPI callbacks to cmp_* buffers)
    trader_handler_->verify_mode = true;
    trader_handler_->cmp_position_count = 0;
    trader_handler_->cmp_account_count = 0;
    trader_handler_->cmp_order_count = 0;

    // Drain cmp buffers from any previous run
    { PositionRecord dummy; while (trader_handler_->cmp_positions.tryPop(dummy)) {} }
    { AccountRecord dummy;   while (trader_handler_->cmp_accounts.tryPop(dummy)) {} }
    { OrderRecord dummy;     while (trader_handler_->cmp_orders.tryPop(dummy)) {} }

    // Submit queries (flow_ctrl is idle at this point)
    LOG_INFO("[Verify] Querying CTP for positions, account, orders...");
    queryPosition();
    queryAccount();
    queryOrder();

    // Wait for all queries to complete (flow_ctrl serializes, ~1s each)
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // ---- Read remote data ----
    std::vector<PositionRecord> remote_positions;
    std::vector<AccountRecord>   remote_accounts;
    std::vector<OrderRecord>     remote_orders;

    PositionRecord rp;
    while (trader_handler_->cmp_positions.tryPop(rp)) remote_positions.push_back(rp);
    AccountRecord ra;
    while (trader_handler_->cmp_accounts.tryPop(ra)) remote_accounts.push_back(ra);
    OrderRecord ro;
    while (trader_handler_->cmp_orders.tryPop(ro))   remote_orders.push_back(ro);

    trader_handler_->verify_mode = false;

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
    LOG_INFO("[Compare] POSITION: local={} remote={}",
             [&](){ std::vector<PositionRecord> v; position_mgr_.getAllPositions(v); return v.size(); }(),
             remote_positions.size());

    for (const auto& rp : remote_positions) {
        auto* lp = position_mgr_.getPosition(rp.instrument, rp.posi_direction, rp.position_date);
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
    // Check for local-only positions
    std::vector<PositionRecord> local_positions;
    position_mgr_.getAllPositions(local_positions);
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
             [&](){ int n=0; order_mgr_.forEachActive([&](auto&){ n++; }); return n; }(),
             remote_orders.size());
    for (const auto& ro : remote_orders) {
        std::string ref(ro.order_ref.data());
        auto key = std::to_string(ro.front_id) + "_" + std::to_string(ro.session_id) + "_" + ref;
        auto* lo = order_mgr_.findByOrderRef(key);
        if (!lo && ro.order_sys_id[0] != '\0') lo = order_mgr_.findByOrderSysID(std::string(ro.order_sys_id.data()));
        if (lo) {
            bool ok = (lo->status == ro.status && lo->volume_traded == ro.volume_traded
                       && lo->volume_total == ro.volume_total);
            LOG_INFO("[Compare]   {} ref={} {} status={}({}) filled={}/{}({}/{})",
                     ro.request.instrument, ref, ok?"OK":"!!",
                     static_cast<char>(lo->status), static_cast<char>(ro.status),
                     lo->volume_traded, lo->request.volume,
                     ro.volume_traded, ro.volume_total);
        } else {
            LOG_WARN("[Compare]   {} ref={} ONLY-REMOTE status={}",
                     ro.request.instrument, ref, static_cast<char>(ro.status));
        }
    }

    // ---- Compare Trades ----
    LOG_INFO("[Compare] TRADE: local-history={} remote={}",
             trade_history_.size(), remote_trades_.size());
    int t_ok = 0, t_extra = 0;
    for (const auto& rt : remote_trades_) {
        bool found = false;
        for (const auto& lt : trade_history_) {
            if (std::string(lt.trade_id.data()) == std::string(rt.trade_id.data())) {
                found = true;
                if (std::abs(lt.price - rt.price) < 0.01 && lt.volume == rt.volume) t_ok++;
                break;
            }
        }
        if (!found) t_extra++;
    }
    LOG_INFO("[Compare] TRADE: {} matched, {} new-remote (not in local history)",
             t_ok, t_extra);

    // ---- Commission Reconciliation using trade history ----
    double total_comm = 0;
    for (const auto& t : trade_history_) {
        bool isOpen = (t.offset == Offset::Open);
        double comm = position_mgr_.calcCommissionForOrder(
            t.instrument, t.price, t.volume, isOpen, false);
        total_comm += comm;
    }
    auto acc = position_mgr_.getAccount();
    LOG_INFO("[Compare] COMMISSION: local-tracked={:.2f} trade-history-calc={:.2f}",
             acc.commission, total_comm);

    LOG_INFO("========== COMPARISON COMPLETE ==========");
}

} // namespace falcon
