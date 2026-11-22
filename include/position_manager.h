#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "types.h"

namespace falcon {

// Funds/position reservation of one live order. Released proportionally on
// fills, cancels and rejects so concurrent orders never touch each other.
struct OrderFreeze {
    std::string instrument;
    Direction direction = Direction::Buy;
    Offset offset = Offset::Open;
    char pos_dir = '2';                         // position record holding the freeze
    int volume = 0;                             // remaining frozen volume
    double margin = 0.0;                        // remaining frozen margin
    double commission = 0.0;                    // remaining frozen commission
    std::vector<std::pair<char, int>> buckets;  // position_date -> frozen volume
};

class PositionManager {
    // "Instrument|Dir|Date" → record
    std::unordered_map<std::string, PositionRecord> positions_;
    AccountRecord account_;
    bool account_loaded_ = false;

    // Instrument contract info, keyed by instrument id
    std::unordered_map<std::string, InstrumentInfo> instruments_;

    // Commission per instrument (or per product id)
    std::unordered_map<std::string, CommissionInfo> commissions_;

    std::unordered_map<std::string, double> pre_settlement_;
    std::unordered_map<std::string, double> last_price_;
    std::unordered_map<std::string, double> upper_limit_;
    std::unordered_map<std::string, double> lower_limit_;

    // Order key ("FrontID_SessionID_OrderRef") → reservation
    std::unordered_map<std::string, OrderFreeze> order_freezes_;

    mutable std::mutex mtx_;
    char broker_margin_price_type_ = '1';  // '1'=昨结算 '2'=最新价 '3'=均价 '4'=开仓价

    static std::string posKey(const std::string& inst, char dir, char date) {
        return inst + "|" + dir + "|" + date;
    }

    // ---- Helpers below require mtx_ to be held ----
    double calcFuturesMargin(const InstrumentInfo& inst, double price, int vol, char dir) const;
    double calcOptionsMargin(const InstrumentInfo& inst, double price, int vol, bool isYesterday) const;
    double getMarginPrice(const std::string& inst_id, bool isYesterday) const;
    double calcMarginLocked(const std::string& inst_id, double price, int vol,
                            char direction, bool isYesterday) const;
    double calcCommissionLocked(const std::string& inst_id, double price, int vol,
                                bool isOpen, bool isCloseToday) const;
    const CommissionInfo* findCommission(const std::string& inst_id) const;
    std::vector<char> closeDateOrder(const std::string& inst_id, Offset offset) const;
    void releaseLocked(OrderFreeze& fr, int vol);
    void recalcAccount();

public:
    // ======= Instrument Management =======
    void addInstrument(const InstrumentInfo& info);
    void setCommission(const std::string& key, const CommissionInfo& comm);
    void setBrokerMarginPriceType(char t);
    void setCommissionBaseline(double comm);
    void setPreSettlement(const std::string& inst_id, double px);
    void updateLastPrice(const std::string& inst_id, double px);
    std::optional<InstrumentInfo> getInstrument(const std::string& id) const;

    // ======= Order Lifecycle: Freeze/Unfreeze =======
    // Freeze margin/commission/position for an order before ReqOrderInsert.
    // Returns false if the account is loaded and funds are insufficient.
    bool onOrderRequest(const std::string& order_key, const OrderRequest& req);
    // Release up to `volume` lots of the order's reservation (cancel/reject).
    void releaseOrder(const std::string& order_key, int volume);
    std::optional<OrderFreeze> getOrderFreeze(const std::string& order_key) const;

    // ======= Trade Fill =======
    // order_key may be empty for trades of orders not sent by this gateway.
    void onTradeFill(const TradeRecord& trade, const std::string& order_key = {});

    // ======= Tick → Position Profit =======
    void onTick(const TickData& tick);

    // ======= Bulk Query =======
    void clearPositions();
    void onQueryPosition(const PositionRecord& pos);
    void onQueryAccount(const AccountRecord& acc);

    // ======= Query =======
    std::optional<PositionRecord> getPosition(const std::string& inst, char dir, char date) const;
    AccountRecord getAccount() const;
    double getAvailable() const;
    bool isAccountLoaded() const;
    void getAllPositions(std::vector<PositionRecord>& out) const;

    double calcMargin(const std::string& inst_id, double price, int vol,
                      char direction, bool isYesterday) const;
    double calcCommissionForOrder(const std::string& inst_id, double price,
                                  int vol, bool isOpen, bool isCloseToday) const;

    void reset();
};

} // namespace falcon
