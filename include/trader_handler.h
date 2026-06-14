#pragma once
#include <atomic>
#include <functional>
#include <string>
#include "ctp/ThostFtdcTraderApi.h"
#include "ring_buffer.h"
#include "flow_controller.h"
#include "order_manager.h"
#include "position_manager.h"
#include "types.h"

namespace falcon {

// Trader SPI: receives CTP callbacks, pushes to ring buffers for async processing.
// NO blocking, NO heap allocation in SPI callbacks (use pre-allocated struct copies).
class TraderHandler : public CThostFtdcTraderSpi {
public:
    // Ring buffers (SPI thread → worker thread)
    RingBuffer<OrderRecord, 4096> order_updates;
    RingBuffer<TradeRecord, 4096> trade_updates;
    RingBuffer<OrderRecord, 1024> order_query_results;    // OnRspQryOrder
    RingBuffer<TradeRecord, 1024> trade_query_results;    // OnRspQryTrade
    std::atomic<int> order_query_count{0};                // total count for verification
    std::atomic<int> trade_query_count{0};
    RingBuffer<PositionRecord, 1024> position_results;
    RingBuffer<AccountRecord, 32> account_results;

    // Verification-only buffers (not consumed by trade worker)
    std::atomic<bool> verify_mode{false};
    RingBuffer<PositionRecord, 1024> cmp_positions;
    RingBuffer<AccountRecord, 8>     cmp_accounts;
    RingBuffer<OrderRecord, 1024>    cmp_orders;
    std::atomic<int> cmp_position_count{0};
    std::atomic<int> cmp_account_count{0};
    std::atomic<int> cmp_order_count{0};

    // Signal flags (atomic, no queue needed for simple events)
    std::atomic<bool> front_connected{false};
    std::atomic<bool> front_disconnected{false};
    std::atomic<int> disconnect_reason{0};
    std::atomic<bool> authenticated{false};
    std::atomic<bool> login_success{false};
    std::atomic<int> login_error_id{0};

    // Error callback (called from worker thread)
    std::function<void(int, const std::string&)> on_error;

    // Flow controller reference (shared with Gateway)
    FlowController* flow_ctrl = nullptr;

    // Saved login info
    int front_id = 0;
    int session_id = 0;
    char trading_day[10] = {};
    int max_order_ref = 0;

    // Order/position managers (accessed only from worker thread)
    OrderManager* order_mgr = nullptr;
    PositionManager* position_mgr = nullptr;

    // Instrument callback (called from SPI → worker thread via gateway)
    std::function<void(CThostFtdcInstrumentField*, bool)> on_instrument;
    // Commission rate callback
    std::function<void(const std::string& inst_id, double open_money, double open_vol,
                       double close_money, double close_vol, double close_today_money,
                       double close_today_vol)> on_commission_rate;
    // Broker trading params callback (MarginPriceType etc)
    std::function<void(char margin_price_type)> on_broker_params;

private:
    // Internal state
    std::atomic<bool> need_relogin_{true};
    std::string login_error_msg_;

    char error_buf_[256] = {};

public:
    TraderHandler();
    ~TraderHandler() = default;

    // CTP SPI overrides (called in CTP internal thread)
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                   CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                 CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                            CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField* pSettlementInfo,
                                 CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                     CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* pCommRate,
                                           CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField* pParams,
                                      CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo,
                       int nRequestID, bool bIsLast) override;
    void OnRspQryTrade(CThostFtdcTradeField* pTrade, CThostFtdcRspInfoField* pRspInfo,
                       int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) override;

    // Control
    bool shouldRelogin() const { return need_relogin_; }
};

} // namespace falcon
