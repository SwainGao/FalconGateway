#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include "ctp_api.h"
#include "ring_buffer.h"
#include "flow_controller.h"
#include "types.h"

namespace falcon {

// Order and trade pushes share one queue so the worker sees them in CTP order
// (an OnRtnOrder carrying OrderSysID always precedes its OnRtnTrade).
struct TraderEvent {
    enum class Kind : unsigned char { Order, Trade };
    Kind kind = Kind::Order;
    OrderRecord order;
    TradeRecord trade;
};

// Trader SPI: receives CTP callbacks, pushes to ring buffers for async processing.
class TraderHandler final : public CThostFtdcTraderSpi {
public:
    // Ring buffers (SPI thread → worker thread). Trading events are never
    // dropped: when a buffer is full the SPI thread waits for the worker.
    RingBuffer<TraderEvent, 8192> events;
    RingBuffer<PositionRecord, 1024> position_results;
    RingBuffer<AccountRecord, 32> account_results;
    RingBuffer<TradeRecord, 4096> trade_query_results;    // OnRspQryTrade (history)
    std::atomic<int> order_query_count{0};
    std::atomic<int> trade_query_count{0};
    std::atomic<long> buffer_full_waits{0};

    // Verification: responses to the query whose request id equals
    // verify_request_id go to the cmp_* buffers instead.
    std::atomic<int> verify_request_id{-1};
    RingBuffer<PositionRecord, 1024> cmp_positions;
    RingBuffer<AccountRecord, 8>     cmp_accounts;
    RingBuffer<OrderRecord, 1024>    cmp_orders;
    RingBuffer<TradeRecord, 4096>    cmp_trades;

    // Signal flags (atomic, no queue needed for simple events)
    std::atomic<bool> front_connected{false};
    std::atomic<bool> front_disconnected{false};
    std::atomic<int> disconnect_reason{0};
    std::atomic<bool> authenticated{false};
    std::atomic<bool> login_success{false};
    std::atomic<int> login_error_id{0};
    std::atomic<bool> stopping{false};   // stop blocking pushes during shutdown

    // Called from the SPI thread
    std::function<void(int, const std::string&)> on_error;

    FlowController* flow_ctrl = nullptr;

    // Login info (written by SPI thread before login_success is set)
    std::atomic<int> front_id{0};
    std::atomic<int> session_id{0};
    std::atomic<int> max_order_ref{0};

    // Synchronous SPI-thread callbacks; arguments are copies owned by the handler.
    std::function<void(const InstrumentInfo* inst, bool is_last)> on_instrument;
    std::function<void(const std::string& inst_id, const CommissionInfo& comm)> on_commission_rate;
    std::function<void(char margin_price_type)> on_broker_params;

    TraderHandler() = default;
    ~TraderHandler() = default;

    std::string tradingDay() const;
    void resetState();
    bool shouldRelogin() const { return need_relogin_; }

    // CTP SPI overrides (called in CTP internal thread)
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                          CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                             CThostFtdcRspInfoField* pRspInfo) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                          CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                             CThostFtdcRspInfoField* pRspInfo) override;
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

private:
    std::atomic<bool> need_relogin_{true};
    mutable std::mutex info_mtx_;
    std::string trading_day_;

    template<typename Buffer, typename T>
    void pushReliable(Buffer& buf, T&& value, const char* what);
    void completeQuery(int request_id, bool is_last, CThostFtdcRspInfoField* info, const char* what);
    bool isVerify(int request_id) const { return request_id == verify_request_id.load(); }
    void pushInsertReject(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo);
};

} // namespace falcon
