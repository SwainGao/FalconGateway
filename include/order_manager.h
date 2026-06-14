#pragma once
#include <unordered_map>
#include <string>
#include <string_view>
#include <mutex>
#include "types.h"

namespace falcon {

// Tracks all orders by their unique keys.
// Thread-safe: single writer (worker thread), concurrent readers OK with shared_lock.
class OrderManager {
    // Phase 1 key: "FrontID_SessionID_OrderRef" (before exchange accepts)
    std::unordered_map<std::string, OrderRecord> pending_;

    // Phase 2 key: "OrderSysID" (after exchange assigns sys_id)
    std::unordered_map<std::string, OrderRecord*> exchange_lookup_;

    // Raw storage for stable pointers
    std::unordered_map<std::string, OrderRecord> storage_;

    mutable std::recursive_mutex mtx_;

    int order_ref_counter_ = 1000;
    std::string makeOrderRef();

public:
    // Create new order on ReqOrderInsert
    OrderRecord* create(const OrderRequest& req, int front_id, int session_id, const std::string& order_ref);

    // Update order on OnRtnOrder callback
    // Returns the updated record and whether it became final
    struct UpdateResult {
        OrderRecord* record;
        bool newly_final = false;    // 刚变为终态
        bool is_cancel = false;       // 是撤单
        bool is_exchange_reject = false; // 交易所拒单
    };
    UpdateResult update(int front_id, int session_id,
                        const std::string& order_ref,
                        const std::string& order_sys_id,
                        char order_status, char submit_status,
                        int volume_traded, int volume_total,
                        const std::string& status_msg);

    // Lookup
    OrderRecord* findByOrderRef(const std::string& key);
    OrderRecord* findByOrderSysID(const std::string& sys_id);

    // Traverse all active (non-final) orders
    template<typename F>
    void forEachActive(F&& fn) {
        std::lock_guard lock(mtx_);
        for (auto& [k, v] : storage_) {
            if (!v.isFinal()) fn(v);
        }
    }

    std::string nextOrderRef();
};

} // namespace falcon
