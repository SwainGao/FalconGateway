#pragma once
#include <atomic>
#include "ctp_api.h"
#include "ring_buffer.h"
#include "types.h"

namespace falcon {

// Market Data SPI: receives CTP callbacks, pushes ticks to ring buffer.
// Ticks are dropped (and counted) when the buffer is full.
class MdHandler final : public CThostFtdcMdSpi {
public:
    RingBuffer<TickData, 65536> tick_buffer;

    std::atomic<bool> front_connected{false};
    std::atomic<bool> front_disconnected{false};
    std::atomic<bool> login_success{false};
    std::atomic<long> dropped_ticks{0};

    MdHandler() = default;
    ~MdHandler() = default;

    void resetState();

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                             CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
};

} // namespace falcon
