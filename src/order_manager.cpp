#include "order_manager.h"

namespace falcon {

std::string OrderManager::makeKey(int front_id, int session_id, std::string_view order_ref) {
    std::string key = std::to_string(front_id) + "_" + std::to_string(session_id) + "_";
    key.append(order_ref.data(), strnlen(order_ref.data(), order_ref.size()));
    return key;
}

std::string OrderManager::sysKey(std::string_view exchange, std::string_view sys_id) {
    std::string key(exchange);
    key += '|';
    key += sys_id;
    return key;
}

bool OrderManager::create(const OrderRequest& req, int front_id, int session_id,
                          const std::string& order_ref) {
    OrderRecord rec;
    rec.request = req;
    rec.front_id = front_id;
    rec.session_id = session_id;
    size_t len = std::min(order_ref.size(), rec.order_ref.size() - 1);
    std::copy_n(order_ref.begin(), len, rec.order_ref.begin());
    rec.volume_total = req.volume;
    rec.create_time = now();
    rec.update_time = rec.create_time;
    rec.is_local = true;

    auto key = makeKey(front_id, session_id, rec.order_ref.data());
    std::lock_guard lock(mtx_);
    return storage_.emplace(std::move(key), std::move(rec)).second;
}

bool OrderManager::remove(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = storage_.find(key);
    if (it == storage_.end()) return false;
    if (it->second.order_sys_id[0] != '\0') {
        exchange_lookup_.erase(sysKey(it->second.request.exchange, it->second.order_sys_id.data()));
    }
    storage_.erase(it);
    return true;
}

OrderManager::UpdateResult OrderManager::update(const OrderRecord& rtn) {
    UpdateResult result;
    std::string sys_id(rtn.order_sys_id.data());

    std::lock_guard lock(mtx_);
    auto key = makeKey(rtn.front_id, rtn.session_id, rtn.order_ref.data());
    auto it = storage_.find(key);
    if (it == storage_.end() && !sys_id.empty()) {
        auto sys_it = exchange_lookup_.find(sysKey(rtn.request.exchange, sys_id));
        if (sys_it != exchange_lookup_.end()) {
            key = sys_it->second;
            it = storage_.find(key);
        }
    }
    if (it == storage_.end()) return result;  // external order (other session)

    auto& ord = it->second;
    result.found = true;
    result.key = key;

    // Final states are terminal; fill counts never go backwards. This makes
    // replays (THOST_TERT_RESTART) and duplicate pushes harmless.
    if (ord.isFinal() || rtn.volume_traded < ord.volume_traded) {
        result.ignored = true;
        result.record = ord;
        return result;
    }

    ord.update_time = now();
    ord.status = rtn.status;
    ord.submit_status = rtn.submit_status;
    ord.volume_traded = rtn.volume_traded;
    ord.volume_total = rtn.volume_total;

    if (!sys_id.empty() && ord.order_sys_id[0] == '\0') {
        ord.order_sys_id = rtn.order_sys_id;
        exchange_lookup_[sysKey(ord.request.exchange, sys_id)] = key;
    }

    result.newly_final = ord.isFinal();
    result.record = ord;
    return result;
}

std::optional<OrderRecord> OrderManager::findByKey(const std::string& key) const {
    std::lock_guard lock(mtx_);
    auto it = storage_.find(key);
    if (it == storage_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> OrderManager::keyByOrderSysID(const std::string& exchange,
                                                         const std::string& sys_id) const {
    std::lock_guard lock(mtx_);
    auto it = exchange_lookup_.find(sysKey(exchange, sys_id));
    if (it == exchange_lookup_.end()) return std::nullopt;
    return it->second;
}

std::optional<OrderRecord> OrderManager::findByOrderSysID(const std::string& exchange,
                                                          const std::string& sys_id) const {
    std::lock_guard lock(mtx_);
    auto it = exchange_lookup_.find(sysKey(exchange, sys_id));
    if (it == exchange_lookup_.end()) return std::nullopt;
    auto ord = storage_.find(it->second);
    if (ord == storage_.end()) return std::nullopt;
    return ord->second;
}

std::vector<OrderRecord> OrderManager::activeOrders() const {
    std::lock_guard lock(mtx_);
    std::vector<OrderRecord> out;
    for (const auto& [k, v] : storage_) {
        if (!v.isFinal()) out.push_back(v);
    }
    return out;
}

size_t OrderManager::size() const {
    std::lock_guard lock(mtx_);
    return storage_.size();
}

} // namespace falcon
