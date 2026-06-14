#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include <vector>
#include "types.h"

namespace falcon {

class PositionManager {
    // "Instrument|Dir|Date" → record
    std::unordered_map<std::string, PositionRecord> positions_;
    AccountRecord account_;

    // Full instrument contract info
    std::unordered_map<std::string, InstrumentInfo> instruments_;

    // Commission per instrument (may store both inst-level and product-level keys)
    std::unordered_map<std::string, CommissionInfo> commissions_;

    // Pre-settlement price per instrument
    std::unordered_map<std::string, double> pre_settlement_;
    std::unordered_map<std::string, double> last_price_;

    mutable std::recursive_mutex mtx_;
    char broker_margin_price_type_ = '1';  // '1'=昨结算 '2'=最新价 '3'=均价 '4'=开仓价

    static std::string posKey(const std::string& inst, char dir, char date) {
        return inst + "|" + dir + "|" + date;
    }

    // Margin calc helpers
    double calcFuturesMargin(const InstrumentInfo& inst, double price, int vol, char dir) const;
    double calcOptionsMargin(const InstrumentInfo& inst, double price, int vol, bool isYesterday) const;

    // Commission calc
    double calcCommission(const std::string& inst_id, double price, int vol,
                          bool isOpen, bool isCloseToday) const;

    // Get effective margin price (depending on MarginPriceType and position date)
    double getMarginPrice(const InstrumentInfo& inst, const std::string& inst_id,
                          bool isYesterday) const;

public:
    // ======= Instrument Management =======
    void addInstrument(const InstrumentInfo& info);
    void setCommission(const std::string& key, const CommissionInfo& comm);
    void setBrokerMarginPriceType(char t) { broker_margin_price_type_ = t; }
    void setCommissionBaseline(double comm) { account_.commission = comm; }
    void setPreSettlement(const std::string& inst_id, double px);
    void updateLastPrice(const std::string& inst_id, double px);
    const InstrumentInfo* getInstrument(const std::string& id) const;

    // ======= Order Lifecycle: Freeze/Unfreeze =======
    // Freeze before ReqOrderInsert. Returns false if margin insufficient.
    bool onOrderRequest(const OrderRequest& req);
    // Unfreeze on reject/cancel. unfilledVolume = original volume - already traded.
    void onOrderRejectOrCancel(const OrderRequest& req, int unfilledVolume);

    // ======= Trade Fill =======
    void onTradeFill(const TradeRecord& trade);

    // ======= Tick → Position Profit =======
    void onTick(const TickData& tick);

    // ======= Bulk Query =======
    void onQueryPosition(const PositionRecord& pos);
    void onQueryAccount(const AccountRecord& acc);

    // ======= Query =======
    PositionRecord* getPosition(const std::string& inst, char dir, char date);
    AccountRecord getAccount() const;
    double getAvailable() const;
    void getAllPositions(std::vector<PositionRecord>& out) const;

    // Public margin/commission API
    double calcMargin(const std::string& inst_id, double price, int vol,
                      char direction, bool isYesterday) const;
    double calcCommissionForOrder(const std::string& inst_id, double price,
                                  int vol, bool isOpen, bool isCloseToday) const;

    void reset();
};

} // namespace falcon
