#include "trader_handler.h"
#include "logger.h"
#include <cstring>

namespace falcon {

TraderHandler::TraderHandler() = default;

// ===== SPI Callbacks (CTP internal thread) =====

void TraderHandler::OnFrontConnected() {
    front_connected = true;
}

void TraderHandler::OnFrontDisconnected(int nReason) {
    front_connected = false;
    front_disconnected = true;
    disconnect_reason = nReason;
}

void TraderHandler::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                                       CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID == 0) {
        authenticated = true;
    } else if (pRspInfo) {
        need_relogin_ = false;
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID == 0) {
        front_id = pRspUserLogin->FrontID;
        session_id = pRspUserLogin->SessionID;
        std::memcpy(trading_day, pRspUserLogin->TradingDay, sizeof(trading_day));
        max_order_ref = std::stoi(pRspUserLogin->MaxOrderRef);
        login_success = true;
        need_relogin_ = true;
    } else if (pRspInfo) {
        login_success = false;
        login_error_id = pRspInfo->ErrorID;
        if (pRspInfo->ErrorID != 5) {  // Not "duplicate login"
            need_relogin_ = false;
        }
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                      CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[SPI] OnRspOrderInsert: {} {}", pRspInfo->ErrorID,
                  (pInputOrder ? pInputOrder->InstrumentID : "?"));
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void TraderHandler::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;

    OrderRecord rec;
    rec.front_id = pOrder->FrontID;
    rec.session_id = pOrder->SessionID;
    std::memcpy(rec.order_ref.data(), pOrder->OrderRef, sizeof(rec.order_ref) - 1);
    std::memcpy(rec.order_sys_id.data(), pOrder->OrderSysID, sizeof(rec.order_sys_id) - 1);
    rec.status = static_cast<OrderStatus>(pOrder->OrderStatus);
    rec.submit_status = pOrder->OrderSubmitStatus;
    rec.volume_traded = pOrder->VolumeTraded;
    rec.volume_total = pOrder->VolumeTotal;
    rec.is_local = (pOrder->FrontID == front_id && pOrder->SessionID == session_id);
    rec.update_time = now();

    rec.request.instrument = pOrder->InstrumentID;
    rec.request.exchange = pOrder->ExchangeID;
    rec.request.direction = static_cast<Direction>(pOrder->Direction);
    rec.request.offset = static_cast<Offset>(pOrder->CombOffsetFlag[0]);
    rec.request.price = pOrder->LimitPrice;
    rec.request.volume = pOrder->VolumeTotalOriginal;

    if (!order_updates.tryPush(std::move(rec))) {
        LOG_CRITICAL("[SPI] Order update ring buffer FULL! Dropping order.");
        if (on_error) on_error(-1, "Order update buffer full");
    }
}

void TraderHandler::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;

    TradeRecord rec;
    std::memcpy(rec.trade_id.data(), pTrade->TradeID, sizeof(rec.trade_id) - 1);
    std::memcpy(rec.order_sys_id.data(), pTrade->OrderSysID, sizeof(rec.order_sys_id) - 1);
    rec.instrument = pTrade->InstrumentID;
    rec.exchange = pTrade->ExchangeID;
    rec.direction = static_cast<Direction>(pTrade->Direction);
    rec.offset = static_cast<Offset>(pTrade->OffsetFlag);
    rec.price = pTrade->Price;
    rec.volume = pTrade->Volume;
    rec.trade_time = now();

    if (!trade_updates.tryPush(std::move(rec))) {
        LOG_CRITICAL("[SPI] Trade update ring buffer FULL! Dropping trade.");
        if (on_error) on_error(-1, "Trade update buffer full");
    }
}

void TraderHandler::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pos,
                                              CThostFtdcRspInfoField* pRspInfo,
                                              int nRequestID, bool bIsLast) {
    if (!pos) {
        if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
        return;
    }

    PositionRecord rec;
    rec.instrument = pos->InstrumentID;
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

    if (verify_mode) {
        cmp_positions.tryPush(std::move(rec));
        cmp_position_count++;
    } else {
        position_results.tryPush(std::move(rec));
    }

    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQryTradingAccount(CThostFtdcTradingAccountField* acc,
                                            CThostFtdcRspInfoField* pRspInfo,
                                            int nRequestID, bool bIsLast) {
    if (!acc) {
        if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
        return;
    }

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

    if (verify_mode) {
        cmp_accounts.tryPush(std::move(rec));
        cmp_account_count++;
    } else {
        account_results.tryPush(std::move(rec));
    }

    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQryInstrument(CThostFtdcInstrumentField* inst,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID, bool bIsLast) {
    if (on_instrument) on_instrument(inst, bIsLast);
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField* pSettlementInfo,
                                            CThostFtdcRspInfoField* pRspInfo,
                                            int nRequestID, bool bIsLast) {
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                                CThostFtdcRspInfoField* pRspInfo,
                                                int nRequestID, bool bIsLast) {
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                                   CThostFtdcRspInfoField* pRspInfo,
                                                   int nRequestID, bool bIsLast) {
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQryBrokerTradingParams(
    CThostFtdcBrokerTradingParamsField* pParams,
    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pParams && on_broker_params) {
        on_broker_params(pParams->MarginPriceType);
    }
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQryInstrumentCommissionRate(
    CThostFtdcInstrumentCommissionRateField* pCommRate,
    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pCommRate && on_commission_rate) {
        on_commission_rate(pCommRate->InstrumentID,
                          pCommRate->OpenRatioByMoney, pCommRate->OpenRatioByVolume,
                          pCommRate->CloseRatioByMoney, pCommRate->CloseRatioByVolume,
                          pCommRate->CloseTodayRatioByMoney, pCommRate->CloseTodayRatioByVolume);
    }
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[SPI] OnRspError: id={} msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        if (on_error) on_error(pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
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
        OrderRecord rec;
        rec.front_id = pOrder->FrontID;
        rec.session_id = pOrder->SessionID;
        std::memcpy(rec.order_ref.data(), pOrder->OrderRef, sizeof(rec.order_ref) - 1);
        std::memcpy(rec.order_sys_id.data(), pOrder->OrderSysID, sizeof(rec.order_sys_id) - 1);
        rec.status = static_cast<OrderStatus>(pOrder->OrderStatus);
        rec.submit_status = pOrder->OrderSubmitStatus;
        rec.volume_traded = pOrder->VolumeTraded;
        rec.volume_total = pOrder->VolumeTotal;
        rec.request.instrument = pOrder->InstrumentID;
        rec.request.exchange = pOrder->ExchangeID;
        rec.request.direction = static_cast<Direction>(pOrder->Direction);
        rec.request.offset = static_cast<Offset>(pOrder->CombOffsetFlag[0]);
        rec.request.price = pOrder->LimitPrice;
        rec.request.volume = pOrder->VolumeTotalOriginal;
        if (verify_mode) {
            cmp_orders.tryPush(std::move(rec));
            cmp_order_count++;
        } else {
            order_query_results.tryPush(std::move(rec));
        }
    }
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

void TraderHandler::OnRspQryTrade(CThostFtdcTradeField* pTrade,
                                   CThostFtdcRspInfoField* pRspInfo,
                                   int nRequestID, bool bIsLast) {
    if (pTrade) {
        trade_query_count++;
        TradeRecord rec;
        std::memcpy(rec.trade_id.data(), pTrade->TradeID, sizeof(rec.trade_id) - 1);
        std::memcpy(rec.order_sys_id.data(), pTrade->OrderSysID, sizeof(rec.order_sys_id) - 1);
        rec.instrument = pTrade->InstrumentID;
        rec.exchange = pTrade->ExchangeID;
        rec.direction = static_cast<Direction>(pTrade->Direction);
        rec.offset = static_cast<Offset>(pTrade->OffsetFlag);
        rec.price = pTrade->Price;
        rec.volume = pTrade->Volume;
        trade_query_results.tryPush(std::move(rec));
    }
    if (bIsLast && flow_ctrl) flow_ctrl->onResponseComplete();
}

} // namespace falcon
