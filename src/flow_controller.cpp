#include "flow_controller.h"
#include "logger.h"

namespace falcon {

FlowController::~FlowController() { stop(); }

void FlowController::setInterval(int ms) { std::lock_guard lk(mtx_); interval_ms_ = ms; }
void FlowController::setTimeout(int ms)  { std::lock_guard lk(mtx_); timeout_ms_ = ms; }
void FlowController::setMaxRetries(int n) { std::lock_guard lk(mtx_); max_retries_ = n; }

void FlowController::start() {
    std::lock_guard lk(mtx_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&FlowController::run, this);
}

void FlowController::stop() {
    {
        std::lock_guard lk(mtx_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lk(mtx_);
    pending_.clear();
    in_flight_id_ = 0;
}

void FlowController::submit(Query query, std::string name) {
    {
        std::lock_guard lk(mtx_);
        pending_.push_back(Item{std::move(query), nullptr, std::move(name), 0});
    }
    cv_.notify_all();
}

void FlowController::submitMarker(Marker marker) {
    {
        std::lock_guard lk(mtx_);
        pending_.push_back(Item{nullptr, std::move(marker), {}, 0});
    }
    cv_.notify_all();
}

bool FlowController::onResponseComplete(int request_id) {
    {
        std::lock_guard lk(mtx_);
        if (in_flight_id_ == 0 || request_id != in_flight_id_) return false;
        in_flight_id_ = 0;
    }
    cv_.notify_all();
    return true;
}

void FlowController::reset() {
    {
        std::lock_guard lk(mtx_);
        pending_.clear();
        in_flight_id_ = 0;
        ++generation_;
    }
    cv_.notify_all();
}

bool FlowController::allDone() const {
    std::lock_guard lk(mtx_);
    return pending_.empty() && in_flight_id_ == 0 && !busy_;
}

int FlowController::inFlightRequestId() const {
    std::lock_guard lk(mtx_);
    return in_flight_id_;
}

int FlowController::timeoutCount() const {
    std::lock_guard lk(mtx_);
    return timeouts_;
}

void FlowController::run() {
    std::unique_lock lk(mtx_);
    while (running_) {
        if (in_flight_id_ != 0) {
            int id = in_flight_id_;
            bool done = cv_.wait_until(lk, in_flight_deadline_, [&] {
                return !running_ || in_flight_id_ != id;
            });
            if (!done) {
                ++timeouts_;
                in_flight_id_ = 0;
                LOG_WARN("[FlowCtrl] Query request_id={} timed out", id);
            }
            continue;
        }

        if (pending_.empty()) {
            cv_.wait(lk, [&] { return !running_ || !pending_.empty() || in_flight_id_ != 0; });
            continue;
        }

        Item item = std::move(pending_.front());
        pending_.pop_front();
        unsigned gen = generation_;
        busy_ = true;  // item is out of the queue but not finished

        if (item.marker) {
            lk.unlock();
            item.marker();
            lk.lock();
            busy_ = false;
            continue;
        }

        if (has_last_query_) {
            auto ready_at = last_query_ + std::chrono::milliseconds(interval_ms_);
            cv_.wait_until(lk, ready_at, [&] { return !running_ || generation_ != gen; });
            if (!running_) break;
            if (generation_ != gen) {  // dropped by reset()
                busy_ = false;
                continue;
            }
        }

        int id = next_id_++;
        in_flight_id_ = id;
        last_query_ = falcon::now();
        has_last_query_ = true;
        in_flight_deadline_ = last_query_ + std::chrono::milliseconds(timeout_ms_);

        lk.unlock();
        int ret = item.query ? item.query(id) : 0;
        lk.lock();
        busy_ = false;

        if (ret != 0) {
            if (in_flight_id_ == id) in_flight_id_ = 0;
            if (generation_ == gen && item.retries < max_retries_) {
                ++item.retries;
                LOG_WARN("[FlowCtrl] Query {} returned {}, retry {}/{}",
                         item.name, ret, item.retries, max_retries_);
                pending_.push_front(std::move(item));
            } else {
                LOG_ERROR("[FlowCtrl] Query {} failed with {}, dropped", item.name, ret);
            }
        }
    }
}

} // namespace falcon
