#include "md_handler.h"
#include "logger.h"

namespace falcon {

void MdHandler::resetState() {
    front_connected = false;
    front_disconnected = false;
    login_success = false;
}

void MdHandler::OnFrontConnected() {
    front_connected = true;
}

void MdHandler::OnFrontDisconnected(int nReason) {
    front_connected = false;
    front_disconnected = true;
    LOG_WARN("[MdSPI] Disconnected. Reason=0x{:04X}", nReason);
}

void MdHandler::OnRspUserLogin(CThostFtdcRspUserLoginField*,
                               CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (pRspInfo && pRspInfo->ErrorID == 0) {
        login_success = true;
    } else if (pRspInfo) {
        LOG_ERROR("[MdSPI] Login failed: {} {}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void MdHandler::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
    if (!pData) return;

    TickData tick;
    tick.instrument = ctpString(pData->InstrumentID);
    tick.exchange = ctpString(pData->ExchangeID);
    tick.update_time = ctpString(pData->UpdateTime);
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
        if (dropped_ticks++ % 1000 == 0) {
            LOG_CRITICAL("[MdSPI] Tick buffer FULL! Dropping {} (dropped={})",
                         pData->InstrumentID, dropped_ticks.load());
        }
    }
}

void MdHandler::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                   CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[MdSPI] Subscribe failed: {} {} inst={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg,
                  (pSpecificInstrument ? pSpecificInstrument->InstrumentID : "?"));
    }
}

void MdHandler::OnRspError(CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        LOG_ERROR("[MdSPI] OnRspError: id={} msg={}", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

} // namespace falcon
