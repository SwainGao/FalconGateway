#include "trader_handler.h"
#include "logger.h"
#include <charconv>
#include <chrono>
#include <thread>

namespace falcon {

namespace {

OrderRecord toOrderRecord(const CThostFtdcOrderField& o) {
    OrderRecord rec;
    rec.front_id = o.FrontID;
    rec.session_id = o.SessionID;
    copyCtpField(rec.order_ref, o.OrderRef);
    copyCtpField(rec.order_sys_id, o.OrderSysID);
    rec.status = static_cast<OrderStatus>(o.OrderStatus);
    rec.submit_status = o.OrderSubmitStatus;
    rec.volume_traded = o.VolumeTraded;
    rec.volume_total = o.VolumeTotal;
    rec.update_time = now();
    rec.request.instrument = ctpString(o.InstrumentID);
    rec.request.exchange = ctpString(o.ExchangeID);
    rec.request.direction = static_cast<Direction>(o.Direction);
    rec.request.offset = static_cast<Offset>(o.CombOffsetFlag[0]);
    rec.request.price = o.LimitPrice;
    rec.request.volume = o.VolumeTotalOriginal;
    return rec;
}

TradeRecord toTradeRecord(const CThostFtdcTradeField& t) {
    TradeRecord rec;
    copyCtpField(rec.trade_id, t.TradeID);
    copyCtpField(rec.order_sys_id, t.OrderSysID);
    copyCtpField(rec.order_ref, t.OrderRef);
    rec.instrument = ctpString(t.InstrumentID);
    rec.exchange = ctpString(t.ExchangeID);
    rec.direction = static_cast<Direction>(t.Direction);
    rec.offset = static_cast<Offset>(t.OffsetFlag);
    rec.price = t.Price;
    rec.volume = t.Volume;
    rec.trade_time = now();
    return rec;
}

InstrumentInfo toInstrumentInfo(const CThostFtdcInstrumentField& p) {
    InstrumentInfo info;
    info.instrument_id = ctpString(p.InstrumentID);
    info.product_id = ctpString(p.ProductID);
    info.exchange_id = ctpString(p.ExchangeID);
    info.product_class = p.ProductClass;
    info.volume_multiple = p.VolumeMultiple;
    info.price_tick = p.PriceTick;
    info.long_margin_ratio = p.LongMarginRatio;
    info.short_margin_ratio = p.ShortMarginRatio;
    if (info.long_margin_ratio > 1.0) info.long_margin_ratio /= 100.0;
    if (info.short_margin_ratio > 1.0) info.short_margin_ratio /= 100.0;
    info.position_date_type = p.PositionDateType;
    info.max_margin_side = p.MaxMarginSideAlgorithm;
    info.max_limit_order_volume = p.MaxLimitOrderVolume;
    info.min_limit_order_volume = p.MinLimitOrderVolume;
    info.max_market_order_volume = p.MaxMarketOrderVolume;
    info.min_market_order_volume = p.MinMarketOrderVolume;
    info.is_trading = p.IsTrading;
    info.inst_life_phase = p.InstLifePhase;
    if (info.isOption()) {
        info.strike_price = p.StrikePrice;
        info.options_type = p.OptionsType;
        info.underlying_id = ctpString(p.UnderlyingInstrID);
        info.underlying_multiple = p.UnderlyingMultiple;
    }
    return info;
}

bool isError(const CThostFtdcRspInfoField* info) { return info && info->ErrorID != 0; }

} // namespace

std::string TraderHandler::tradingDay() const {
    std::lock_guard lk(info_mtx_);
    return trading_day_;
}

void TraderHandler::resetState() {
    front_connected = false;
    front_disconnected = false;
    authenticated = false;
    login_success = false;
    login_error_id = 0;
    stopping = false;
    need_relogin_ = true;
    verify_request_id = -1;
}

template<typename Buffer, typename T>
void TraderHandler::pushReliable(Buffer& buf, T&& value, const char* what) {
    // tryPush leaves `value` untouched when the buffer is full
    if (buf.tryPush(std::move(value))) return;
    buffer_full_waits++;
    LOG_CRITICAL("[SPI] {} buffer FULL, waiting for worker", what);
    while (!buf.tryPush(std::move(value))) {
        if (stopping) {
            LOG_ERROR("[SPI] Dropping {} during shutdown", what);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

void TraderHandler::completeQuery(int request_id, bool is_last, CThostFtdcRspInfoField* info,
                                  const char* what) {
    if (isError(info)) {
        LOG_ERROR("[SPI] {} error: id={} msg={}", what, info->ErrorID, info->ErrorMsg);
        if (on_error) on_error(info->ErrorID, info->ErrorMsg);
    }
    if (is_last && flow_ctrl) flow_ctrl->onResponseComplete(request_id);
}

// ===== SPI Callbacks (CTP internal thread) =====

void TraderHandler::OnFrontConnected() {
    front_connected = true;
}

void TraderHandler::OnFrontDisconnected(int nReason) {
    front_connected = false;
    disconnect_reason = nReason;
    front_disconnected = true;
}

void TraderHandler::OnRspAuthenticate(CThostFtdcRspAuthenticateField*,
                                      CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (pRspInfo && pRspInfo->ErrorID == 0) {
        authenticated = true;
    } else if (pRspInfo) {
        need_relogin_ = false;
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                   CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (pRspInfo && pRspInfo->ErrorID == 0 && pRspUserLogin) {
        front_id = pRspUserLogin->FrontID;
        session_id = pRspUserLogin->SessionID;
        {
            std::lock_guard lk(info_mtx_);
            trading_day_ = ctpString(pRspUserLogin->TradingDay);
        }
        std::string ref = ctpString(pRspUserLogin->MaxOrderRef);
        int max_ref = 0;
        auto [ptr, ec] = std::from_chars(ref.data(), ref.data() + ref.size(), max_ref);
        if (ec != std::errc{}) {
            LOG_WARN("[SPI] Invalid MaxOrderRef '{}', starting from 0", ref);
            max_ref = 0;
        }
        max_order_ref = max_ref;
        need_relogin_ = true;
        login_success = true;
    } else if (pRspInfo) {
        login_error_id = pRspInfo->ErrorID;
        if (pRspInfo->ErrorID != 5) {  // Not "duplicate login"
            need_relogin_ = false;
        }
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::pushInsertReject(CThostFtdcInputOrderField* pInputOrder,
                                     CThostFtdcRspInfoField* pRspInfo) {
    if (pRspInfo) {
        LOG_ERROR("[SPI] Order insert rejected: {} {} inst={} ref={}", pRspInfo->ErrorID,
                  pRspInfo->ErrorMsg, (pInputOrder ? pInputOrder->InstrumentID : "?"),
                  (pInputOrder ? pInputOrder->OrderRef : "?"));
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
    if (!pInputOrder) return;

    // CTP sends no OnRtnOrder for orders rejected by the broker front; turn the
    // rejection into a final order update so the reservation gets released.
    TraderEvent ev;
    ev.kind = TraderEvent::Kind::Order;
    auto& rec = ev.order;
    rec.front_id = front_id;
    rec.session_id = session_id;
    copyCtpField(rec.order_ref, pInputOrder->OrderRef);
    rec.status = OrderStatus::Canceled;
    rec.submit_status = THOST_FTDC_OSS_InsertRejected;
    rec.volume_traded = 0;
    rec.volume_total = pInputOrder->VolumeTotalOriginal;
    rec.update_time = now();
    rec.request.instrument = ctpString(pInputOrder->InstrumentID);
    rec.request.exchange = ctpString(pInputOrder->ExchangeID);
    rec.request.direction = static_cast<Direction>(pInputOrder->Direction);
    rec.request.offset = static_cast<Offset>(pInputOrder->CombOffsetFlag[0]);
    rec.request.price = pInputOrder->LimitPrice;
    rec.request.volume = pInputOrder->VolumeTotalOriginal;
    pushReliable(events, std::move(ev), "Order event");
}

void TraderHandler::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                     CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (isError(pRspInfo)) pushInsertReject(pInputOrder, pRspInfo);
}

void TraderHandler::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                        CThostFtdcRspInfoField* pRspInfo) {
    if (isError(pRspInfo)) pushInsertReject(pInputOrder, pRspInfo);
}

void TraderHandler::OnRspOrderAction(CThostFtdcInputOrderActionField* pAction,
                                     CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (isError(pRspInfo)) {
        LOG_ERROR("[SPI] Cancel rejected: {} {} sys_id={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg,
                  (pAction ? pAction->OrderSysID : "?"));
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnErrRtnOrderAction(CThostFtdcOrderActionField* pAction,
                                        CThostFtdcRspInfoField* pRspInfo) {
    if (isError(pRspInfo)) {
        LOG_ERROR("[SPI] Cancel rejected by exchange: {} {} sys_id={}", pRspInfo->ErrorID,
                  pRspInfo->ErrorMsg, (pAction ? pAction->OrderSysID : "?"));
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;
    TraderEvent ev;
    ev.kind = TraderEvent::Kind::Order;
    ev.order = toOrderRecord(*pOrder);
    ev.order.is_local = (pOrder->FrontID == front_id && pOrder->SessionID == session_id);
    pushReliable(events, std::move(ev), "Order event");
}

void TraderHandler::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;
    TraderEvent ev;
    ev.kind = TraderEvent::Kind::Trade;
    ev.trade = toTradeRecord(*pTrade);
    pushReliable(events, std::move(ev), "Trade event");
}

void TraderHandler::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pos,
                                             CThostFtdcRspInfoField* pRspInfo,
                                             int nRequestID, bool bIsLast) {
    if (pos) {
        PositionRecord rec;
        rec.instrument = ctpString(pos->InstrumentID);
        rec.posi_direction = pos->PosiDirection;
        rec.position_date = pos->PositionDate;
        rec.hedge_flag = pos->HedgeFlag;
        rec.position = pos->Position;
        rec.today_position = pos->TodayPosition;
        rec.yd_position = pos->YdPosition;
        rec.long_frozen = pos->LongFrozen;
        rec.short_frozen = pos->ShortFrozen;
        rec.position_cost = pos->PositionCost;
        rec.open_cost = pos->OpenCost;
        rec.margin = pos->UseMargin;
        rec.frozen_margin = pos->FrozenMargin;
        rec.frozen_commission = pos->FrozenCommission;
        rec.commission = pos->Commission;
        rec.close_profit = pos->CloseProfit;
        rec.position_profit = pos->PositionProfit;

        if (isVerify(nRequestID)) pushReliable(cmp_positions, std::move(rec), "Verify position");
        else pushReliable(position_results, std::move(rec), "Position");
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryInvestorPosition");
}

void TraderHandler::OnRspQryTradingAccount(CThostFtdcTradingAccountField* acc,
                                           CThostFtdcRspInfoField* pRspInfo,
                                           int nRequestID, bool bIsLast) {
    if (acc) {
        AccountRecord rec;
        rec.balance = acc->Balance;
        rec.available = acc->Available;
        rec.pre_balance = acc->PreBalance;
        rec.curr_margin = acc->CurrMargin;
        rec.frozen_margin = acc->FrozenMargin;
        rec.frozen_commission = acc->FrozenCommission;
        rec.frozen_cash = acc->FrozenCash;
        rec.delivery_margin = acc->DeliveryMargin;
        rec.reserve = acc->Reserve;
        rec.close_profit = acc->CloseProfit;
        rec.position_profit = acc->PositionProfit;
        rec.commission = acc->Commission;
        rec.deposit = acc->Deposit;
        rec.withdraw = acc->Withdraw;

        if (isVerify(nRequestID)) pushReliable(cmp_accounts, std::move(rec), "Verify account");
        else pushReliable(account_results, std::move(rec), "Account");
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryTradingAccount");
}

void TraderHandler::OnRspQryInstrument(CThostFtdcInstrumentField* inst,
                                       CThostFtdcRspInfoField* pRspInfo,
                                       int nRequestID, bool bIsLast) {
    if (on_instrument) {
        if (inst) {
            InstrumentInfo info = toInstrumentInfo(*inst);
            on_instrument(&info, bIsLast);
        } else {
            on_instrument(nullptr, bIsLast);
        }
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryInstrument");
}

void TraderHandler::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField*,
                                           CThostFtdcRspInfoField* pRspInfo,
                                           int nRequestID, bool bIsLast) {
    completeQuery(nRequestID, bIsLast, pRspInfo, "QrySettlementInfo");
}

void TraderHandler::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                               CThostFtdcRspInfoField* pRspInfo,
                                               int nRequestID, bool bIsLast) {
    completeQuery(nRequestID, bIsLast, pRspInfo, "SettlementInfoConfirm");
}

void TraderHandler::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                                  CThostFtdcRspInfoField* pRspInfo,
                                                  int nRequestID, bool bIsLast) {
    completeQuery(nRequestID, bIsLast, pRspInfo, "QrySettlementInfoConfirm");
}

void TraderHandler::OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField* pParams,
                                                CThostFtdcRspInfoField* pRspInfo,
                                                int nRequestID, bool bIsLast) {
    if (pParams && on_broker_params) on_broker_params(pParams->MarginPriceType);
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryBrokerTradingParams");
}

void TraderHandler::OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* pCommRate,
                                                     CThostFtdcRspInfoField* pRspInfo,
                                                     int nRequestID, bool bIsLast) {
    if (pCommRate && on_commission_rate) {
        CommissionInfo comm;
        comm.open_ratio_by_money = pCommRate->OpenRatioByMoney;
        comm.open_ratio_by_volume = pCommRate->OpenRatioByVolume;
        comm.close_ratio_by_money = pCommRate->CloseRatioByMoney;
        comm.close_ratio_by_volume = pCommRate->CloseRatioByVolume;
        comm.close_today_ratio_by_money = pCommRate->CloseTodayRatioByMoney;
        comm.close_today_ratio_by_volume = pCommRate->CloseTodayRatioByVolume;
        on_commission_rate(ctpString(pCommRate->InstrumentID), comm);
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryInstrumentCommissionRate");
}

void TraderHandler::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (isError(pRspInfo)) {
        LOG_ERROR("[SPI] OnRspError: req={} id={} msg={}", nRequestID, pRspInfo->ErrorID,
                  pRspInfo->ErrorMsg);
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
    // Frees the flow controller if this error answers the in-flight query
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete(nRequestID);
}

void TraderHandler::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) {
    if (pInstrumentStatus) {
        LOG_DEBUG("[SPI] InstrumentStatus: {} status={}",
                  pInstrumentStatus->InstrumentID, pInstrumentStatus->InstrumentStatus);
    }
}

void TraderHandler::OnRspQryOrder(CThostFtdcOrderField* pOrder,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) {
    if (pOrder) {
        order_query_count++;
        OrderRecord rec = toOrderRecord(*pOrder);
        if (isVerify(nRequestID)) {
            pushReliable(cmp_orders, std::move(rec), "Verify order");
        } else {
            // Treated like a push: OrderManager ignores stale/final duplicates
            TraderEvent ev;
            ev.kind = TraderEvent::Kind::Order;
            ev.order = std::move(rec);
            pushReliable(events, std::move(ev), "Order event");
        }
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryOrder");
}

void TraderHandler::OnRspQryTrade(CThostFtdcTradeField* pTrade,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) {
    if (pTrade) {
        trade_query_count++;
        TradeRecord rec = toTradeRecord(*pTrade);
        if (isVerify(nRequestID)) pushReliable(cmp_trades, std::move(rec), "Verify trade");
        else pushReliable(trade_query_results, std::move(rec), "Trade query");
    }
    completeQuery(nRequestID, bIsLast, pRspInfo, "QryTrade");
}

} // namespace falcon
