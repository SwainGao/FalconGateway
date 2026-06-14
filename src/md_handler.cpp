#include "md_handler.h"
#include "logger.h"
#include <cstring>

namespace falcon {

void MdHandler::OnFrontConnected() {
    front_connected = true;
}

void MdHandler::OnFrontDisconnected(int nReason) {
    front_connected = false;
    LOG_WARN("[MdSPI] Disconnected. Reason=0x{:04X}", nReason);
}

void MdHandler::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                                CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID == 0) {
        login_success = true;
        need_relogin_ = true;
    } else if (pRspInfo) {
        login_success = false;
        LOG_ERROR("[MdSPI] Login failed: {} {}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        if (pRspInfo->ErrorID != 5) {
            need_relogin_ = false;
        }
    }
}

void MdHandler::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
    if (!pData) return;

    TickData tick;
    tick.instrument = pData->InstrumentID;
    tick.exchange = pData->ExchangeID;
    tick.update_time = pData->UpdateTime;
    tick.update_millisec = pData->UpdateMillisec;
    tick.last_price = pData->LastPrice;
    tick.bid_price1 = pData->BidPrice1;
    tick.ask_price1 = pData->AskPrice1;
    tick.bid_volume1 = pData->BidVolume1;
    tick.ask_volume1 = pData->AskVolume1;
    tick.volume = pData->Volume;
    tick.turnover = pData->Turnover;
    tick.open_interest = pData->OpenInterest;
    tick.upper_limit = pData->UpperLimitPrice;
    tick.lower_limit = pData->LowerLimitPrice;
    tick.pre_close = pData->PreClosePrice;
    tick.pre_settlement = pData->PreSettlementPrice;
    tick.pre_open_interest = pData->PreOpenInterest;
    tick.recv_time = now();

    if (!tick_buffer.tryPush(std::move(tick))) {
        LOG_CRITICAL("[MdSPI] Tick buffer FULL! Dropping {}", pData->InstrumentID);
    }
}

void MdHandler::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[MdSPI] Subscribe failed: {} {}",
                  pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void MdHandler::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[MdSPI] OnRspError: id={} msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

} // namespace falcon
