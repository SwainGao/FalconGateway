#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <unordered_set>
#include "ctp/ThostFtdcMdApi.h"
#include "ring_buffer.h"
#include "types.h"

namespace falcon {

// Market Data SPI: receives CTP callbacks, pushes ticks to ring buffer.
class MdHandler : public CThostFtdcMdSpi {
public:
    RingBuffer<TickData, 65536> tick_buffer;

    std::atomic<bool> front_connected{false};
    std::atomic<bool> login_success{false};

    // Newly discovered instruments (from OnRtnDepthMarketData)
    std::unordered_set<std::string> active_instruments;

private:
    std::atomic<bool> need_relogin_{true};

public:
    MdHandler() = default;
    ~MdHandler() = default;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                             CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

    bool shouldRelogin() const { return need_relogin_; }
};

} // namespace falcon
