#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "types.h"

namespace falcon {

// Tracks orders sent by this gateway by "FrontID_SessionID_OrderRef" and by
// "ExchangeID|OrderSysID". Thread-safe; all accessors return copies.
class OrderManager {
    std::unordered_map<std::string, OrderRecord> storage_;
    std::unordered_map<std::string, std::string> exchange_lookup_;  // sys key -> order key
    mutable std::mutex mtx_;

    static std::string sysKey(std::string_view exchange, std::string_view sys_id);

public:
    static std::string makeKey(int front_id, int session_id, std::string_view order_ref);

    // Register a new order before ReqOrderInsert. Returns false if the key exists.
    bool create(const OrderRequest& req, int front_id, int session_id, const std::string& order_ref);

    // Remove an order that never reached CTP (ReqOrderInsert returned non-zero).
    bool remove(const std::string& key);

    struct UpdateResult {
        bool found = false;          // order belongs to this gateway
        bool ignored = false;        // stale/duplicate update (order already final or older fill count)
        bool newly_final = false;    // this update moved the order into a final state
        std::string key;
        OrderRecord record;          // state after the update
    };
    // Apply an OnRtnOrder / OnRspOrderInsert(error) / OnRspQryOrder update.
    UpdateResult update(const OrderRecord& rtn);

    std::optional<OrderRecord> findByKey(const std::string& key) const;
    std::optional<OrderRecord> findByOrderSysID(const std::string& exchange, const std::string& sys_id) const;
    std::optional<std::string> keyByOrderSysID(const std::string& exchange, const std::string& sys_id) const;

    std::vector<OrderRecord> activeOrders() const;
    size_t size() const;
};

} // namespace falcon
