#include "order_manager.h"
#include <cstring>
#include <algorithm>

namespace falcon {

std::string OrderManager::makeOrderRef() {
    std::lock_guard lock(mtx_);
    return std::to_string(++order_ref_counter_);
}

OrderRecord* OrderManager::create(const OrderRequest& req, int front_id, int session_id, const std::string& order_ref) {
    std::lock_guard lock(mtx_);

    OrderRecord rec;
    rec.request = req;
    rec.front_id = front_id;
    rec.session_id = session_id;
    std::copy(order_ref.begin(), order_ref.end(), rec.order_ref.begin());
    rec.create_time = now();
    rec.update_time = rec.create_time;
    rec.is_local = true;

    auto key = rec.orderKey();
    // Trim trailing nulls for lookup key
    auto pos = key.find('\0');
    if (pos != std::string::npos) key.resize(pos);

    auto [it, ok] = storage_.emplace(key, std::move(rec));
    pending_[key] = it->second;
    return &it->second;
}

OrderManager::UpdateResult OrderManager::update(
    int front_id, int session_id,
    const std::string& order_ref,
    const std::string& order_sys_id,
    char order_status, char submit_status,
    int volume_traded, int volume_total,
    const std::string& /*status_msg*/)
{
    std::lock_guard lock(mtx_);
    UpdateResult result{};

    // Find by order_ref key first
    std::string ref_key = std::to_string(front_id) + "_" + std::to_string(session_id) + "_" + order_ref;
    auto it = storage_.find(ref_key);

    // If not found, try lookup by order_sys_id
    if (it == storage_.end() && !order_sys_id.empty()) {
        auto sys_it = exchange_lookup_.find(order_sys_id);
        if (sys_it != exchange_lookup_.end()) {
            ref_key = sys_it->second->orderKey();
            auto pos = ref_key.find('\0');
            if (pos != std::string::npos) ref_key.resize(pos);
            it = storage_.find(ref_key);
        }
    }

    if (it == storage_.end()) {
        // External order (from another session) — create placeholder
        // For now, skip
        return result;
    }

    auto& ord = it->second;
    ord.update_time = now();
    ord.status = static_cast<OrderStatus>(order_status);
    ord.submit_status = submit_status;
    ord.volume_traded = volume_traded;
    ord.volume_total = volume_total;

    // Register in exchange lookup on first sys_id
    if (!order_sys_id.empty() && ord.order_sys_id[0] == '\0') {
        std::copy(order_sys_id.begin(), order_sys_id.end(), ord.order_sys_id.begin());
        exchange_lookup_[order_sys_id] = &ord;
    }

    result.record = &ord;

    if (ord.isFinal()) {
        result.newly_final = true;
        result.is_cancel = (order_status == '5');
        result.is_exchange_reject = (submit_status == '4');
    }

    return result;
}

OrderRecord* OrderManager::findByOrderRef(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = storage_.find(key);
    return it != storage_.end() ? &it->second : nullptr;
}

OrderRecord* OrderManager::findByOrderSysID(const std::string& sys_id) {
    std::lock_guard lock(mtx_);
    auto it = exchange_lookup_.find(sys_id);
    return it != exchange_lookup_.end() ? it->second : nullptr;
}

} // namespace falcon
