#include "position_manager.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace falcon {

// ============================================================
// Instrument Management
// ============================================================
void PositionManager::addInstrument(const InstrumentInfo& info) {
    std::lock_guard lock(mtx_);
    instruments_[info.instrument_id] = info;
}

void PositionManager::setCommission(const std::string& key, const CommissionInfo& comm) {
    std::lock_guard lock(mtx_);
    commissions_[key] = comm;
}

void PositionManager::setBrokerMarginPriceType(char t) {
    std::lock_guard lock(mtx_);
    broker_margin_price_type_ = t;
}

void PositionManager::setCommissionBaseline(double comm) {
    std::lock_guard lock(mtx_);
    account_.commission = comm;
    recalcAccount();
}

void PositionManager::setPreSettlement(const std::string& inst_id, double px) {
    std::lock_guard lock(mtx_);
    pre_settlement_[inst_id] = px;
}

void PositionManager::updateLastPrice(const std::string& inst_id, double px) {
    std::lock_guard lock(mtx_);
    last_price_[inst_id] = px;
}

std::optional<InstrumentInfo> PositionManager::getInstrument(const std::string& id) const {
    std::lock_guard lock(mtx_);
    auto it = instruments_.find(id);
    if (it == instruments_.end()) return std::nullopt;
    return it->second;
}

void PositionManager::recalcAccount() {
    account_.balance = account_.pre_balance + account_.deposit - account_.withdraw
                     + account_.position_profit + account_.close_profit - account_.commission;
    account_.available = account_.balance - account_.curr_margin - account_.frozen_margin
                       - account_.frozen_commission - account_.frozen_cash
                       - account_.delivery_margin - account_.reserve;
}

// ============================================================
// Margin Calculation
// ============================================================

double PositionManager::getMarginPrice(const std::string& inst_id, bool isYesterday) const {
    // Yesterday positions (昨仓) always use PreSettlementPrice
    if (isYesterday) {
        auto it = pre_settlement_.find(inst_id);
        if (it != pre_settlement_.end() && it->second > 0) return it->second;
        return 0.0;
    }
    // Today positions (今仓): depends on broker's MarginPriceType setting
    switch (broker_margin_price_type_) {
        case '1': { // 昨结算价 (simnow default)
            auto it = pre_settlement_.find(inst_id);
            return (it != pre_settlement_.end()) ? it->second : 0.0;
        }
        case '2': { // 最新价
            auto it = last_price_.find(inst_id);
            return (it != last_price_.end()) ? it->second : 0.0;
        }
        case '3':   // 成交均价 — not tracked in real-time, fallback to last
        case '4':   // 开仓价 — per-position, handled in onTradeFill
        default: {
            auto it = last_price_.find(inst_id);
            return (it != last_price_.end()) ? it->second : 0.0;
        }
    }
}

double PositionManager::calcFuturesMargin(const InstrumentInfo& inst, double price,
                                           int vol, char dir) const {
    // 保证金 = (按手数保证金费 + 按金额保证金率 × 价格 × 合约乘数) × 手数
    // Note: 按手数保证金费 currently always 0 for all products
    double ratio = (dir == '2') ? inst.long_margin_ratio : inst.short_margin_ratio;
    if (price <= 0) return 0.0;
    return ratio * price * inst.volume_multiple * static_cast<double>(vol);
}

double PositionManager::calcOptionsMargin(const InstrumentInfo& inst, double price,
                                           int vol, bool isYesterday) const {
    // 期权卖方保证金 = MAX(权利金 + 保证金不变部分, 最小保证金)
    // 权利金: 昨仓用昨结算价; 新仓用价格×合约乘数
    // FixedMargin: 从 ReqQryOptionInstrTradeCost 查询得到
    // MiniMargin: 仅上期所非0 (上期所=昨收盘与昨结算较大值×乘数)

    double premium;
    if (isYesterday) {
        // 昨仓权利金 = 期权昨结算价 × 合约乘数
        // For SHFE: max(昨收盘, 昨结算) × 合约乘数
        // Simplified: use pre_settlement as昨结算
        double settle = 0.0;
        auto it = pre_settlement_.find(inst.instrument_id);
        if (it != pre_settlement_.end()) settle = it->second;
        premium = settle * inst.volume_multiple;
        if (inst.isSHFE() || inst.isINE()) {
            // SHFE/INE: max(收盘, 结算)
            premium = std::max(settle, price) * inst.volume_multiple;
        }
    } else {
        premium = price * inst.volume_multiple;
    }

    double fixed = inst.opt_fixed_margin;
    double mini = inst.opt_mini_margin;

    return std::max(premium + fixed, mini) * static_cast<double>(vol);
}

double PositionManager::calcMarginLocked(const std::string& inst_id, double price,
                                          int vol, char direction, bool isYesterday) const {
    auto it = instruments_.find(inst_id);
    if (it == instruments_.end()) {
        return 0.0;
    }
    const auto& inst = it->second;

    // Resolve effective price based on MarginPriceType (for futures only)
    //   昨仓: always PreSettlementPrice
    //   今仓: depends on broker's MarginPriceType setting
    //     '1' = 昨结算价, '2' = 最新价, '4' = 开仓价 (use caller's price)
    double effective_price = price;
    if (!inst.isOption()) {
        if (isYesterday) {
            double settle_px = getMarginPrice(inst_id, true);
            if (settle_px > 0) effective_price = settle_px;
        } else if (broker_margin_price_type_ == '1' || broker_margin_price_type_ == '2') {
            double type_px = getMarginPrice(inst_id, false);
            if (type_px > 0) effective_price = type_px;
        }
        // type '3' (成交均价) / '4' (开仓价): keep caller's price
    }

    if (inst.isOption()) {
        return calcOptionsMargin(inst, effective_price, vol, isYesterday);
    }
    return calcFuturesMargin(inst, effective_price, vol, direction);
}

double PositionManager::calcMargin(const std::string& inst_id, double price,
                                    int vol, char direction, bool isYesterday) const {
    std::lock_guard lock(mtx_);
    return calcMarginLocked(inst_id, price, vol, direction, isYesterday);
}

// ============================================================
// Commission Calculation
// ============================================================

const CommissionInfo* PositionManager::findCommission(const std::string& inst_id) const {
    auto it = commissions_.find(inst_id);
    if (it != commissions_.end()) return &it->second;
    // Brokers may return rates per product (e.g. "rb") instead of per instrument
    auto inst_it = instruments_.find(inst_id);
    if (inst_it != instruments_.end() && !inst_it->second.product_id.empty()) {
        it = commissions_.find(inst_it->second.product_id);
        if (it != commissions_.end()) return &it->second;
    }
    return nullptr;
}

double PositionManager::calcCommissionForOrder(const std::string& inst_id, double price,
                                                int vol, bool isOpen, bool isCloseToday) const {
    std::lock_guard lock(mtx_);
    return calcCommissionLocked(inst_id, price, vol, isOpen, isCloseToday);
}

double PositionManager::calcCommissionLocked(const std::string& inst_id, double price,
                                              int vol, bool isOpen, bool isCloseToday) const {
    const CommissionInfo* cp = findCommission(inst_id);
    if (!cp) return 0.0;
    const auto& c = *cp;
    auto inst_it = instruments_.find(inst_id);
    double mult = (inst_it != instruments_.end()) ? inst_it->second.volume_multiple : 1.0;

    // 手续费 = 成交数量 × (成交价 × 合约乘数 × OpenRatioByMoney + OpenRatioByVolume)
    double amount = price * mult * static_cast<double>(vol);
    double ratio_by_money, ratio_by_vol;

    if (isOpen) {
        ratio_by_money = c.open_ratio_by_money;
        ratio_by_vol = c.open_ratio_by_volume;
    } else if (isCloseToday) {
        ratio_by_money = c.close_today_ratio_by_money;
        ratio_by_vol = c.close_today_ratio_by_volume;
    } else {
        ratio_by_money = c.close_ratio_by_money;
        ratio_by_vol = c.close_ratio_by_volume;
    }

    return amount * ratio_by_money + static_cast<double>(vol) * ratio_by_vol;
}

// Position date buckets consumed by a close, in order.
//   SHFE/INE: CloseToday → 今仓; CloseYesterday/Close → 昨仓 (exchange treats 平仓 as 平昨)
//   Others:   先开先平, 优先平昨 (normally only the '1' bucket exists)
std::vector<char> PositionManager::closeDateOrder(const std::string& inst_id, Offset offset) const {
    auto it = instruments_.find(inst_id);
    bool by_date = it != instruments_.end()
                && (it->second.isSHFE() || it->second.isINE() || it->second.distinguishesDate());
    if (by_date) {
        if (offset == Offset::CloseToday) return {'1'};
        if (offset == Offset::CloseYesterday) return {'2'};
        return {'2', '1'};
    }
    return {'2', '1'};
}

// ============================================================
// Order Lifecycle: Freeze
// ============================================================

bool PositionManager::onOrderRequest(const std::string& order_key, const OrderRequest& req) {
    std::lock_guard lock(mtx_);

    // A reused key would leak the previous reservation
    if (auto old = order_freezes_.find(order_key); old != order_freezes_.end()) {
        releaseLocked(old->second, old->second.volume);
        order_freezes_.erase(old);
    }

    int vol = req.volume;
    if (vol <= 0) return false;

    auto inst_it = instruments_.find(req.instrument);
    if (inst_it == instruments_.end()) {
        // No instrument info yet — skip freeze, margin will be verified by broker
        return true;
    }
    const auto& inst = inst_it->second;

    bool isOpen = (req.offset == Offset::Open);
    bool isCloseToday = (inst.distinguishesDate() && req.offset == Offset::CloseToday);

    // Market orders freeze at the limit price on the order's side when known
    double order_px = req.price;
    if (req.type == OrderType::Market) {
        const auto& limits = (req.direction == Direction::Buy) ? upper_limit_ : lower_limit_;
        auto lit = limits.find(req.instrument);
        if (lit != limits.end() && lit->second > 0) order_px = lit->second;
    }

    double margin_freeze = 0.0;
    if (isOpen) {
        // calcMarginLocked applies MarginPriceType ('1'/'2' override order_px; '4' keeps it)
        margin_freeze = calcMarginLocked(req.instrument, order_px, vol,
                                         (req.direction == Direction::Buy) ? '2' : '3', false);
    }
    double comm_freeze = calcCommissionLocked(req.instrument, order_px, vol, isOpen, isCloseToday);

    // account_.available already excludes existing reservations
    double total_needed = margin_freeze + comm_freeze;
    if (account_loaded_ && total_needed > 0 && account_.available < total_needed) {
        return false;
    }

    OrderFreeze fr;
    fr.instrument = req.instrument;
    fr.direction = req.direction;
    fr.offset = req.offset;
    fr.volume = vol;
    fr.margin = margin_freeze;
    fr.commission = comm_freeze;

    if (isOpen) {
        fr.pos_dir = (req.direction == Direction::Buy) ? '2' : '3';
        fr.buckets.emplace_back('1', vol);
        auto& pos = positions_[posKey(req.instrument, fr.pos_dir, '1')];
        pos.instrument = req.instrument;
        pos.posi_direction = fr.pos_dir;
        pos.position_date = '1';
    } else {
        // Closing freezes the opposite-side position, split across date buckets
        fr.pos_dir = (req.direction == Direction::Buy) ? '3' : '2';
        int remaining = vol;
        char first_existing = 0;
        for (char d : closeDateOrder(req.instrument, req.offset)) {
            auto it = positions_.find(posKey(req.instrument, fr.pos_dir, d));
            if (it == positions_.end()) continue;
            if (!first_existing) first_existing = d;
            int take = std::min(remaining, std::max(0, it->second.availClose()));
            if (take > 0) {
                fr.buckets.emplace_back(d, take);
                remaining -= take;
            }
            if (remaining == 0) break;
        }
        if (remaining > 0 && first_existing) {
            // Over-close: CTP will reject it; keep the freeze consistent until then
            auto b = std::find_if(fr.buckets.begin(), fr.buckets.end(),
                                  [&](const auto& p) { return p.first == first_existing; });
            if (b != fr.buckets.end()) b->second += remaining;
            else fr.buckets.emplace_back(first_existing, remaining);
        }
    }

    for (const auto& [date, bvol] : fr.buckets) {
        auto it = positions_.find(posKey(req.instrument, fr.pos_dir, date));
        if (it == positions_.end()) continue;
        auto& pos = it->second;
        if (req.direction == Direction::Buy) pos.long_frozen += bvol;
        else pos.short_frozen += bvol;
        double share = static_cast<double>(bvol) / vol;
        pos.frozen_margin += margin_freeze * share;
        pos.frozen_commission += comm_freeze * share;
    }

    account_.frozen_margin += margin_freeze;
    account_.frozen_commission += comm_freeze;
    order_freezes_[order_key] = std::move(fr);

    recalcAccount();
    return true;
}

// ============================================================
// Order Lifecycle: Unfreeze
// ============================================================

void PositionManager::releaseLocked(OrderFreeze& fr, int vol) {
    vol = std::min(vol, fr.volume);
    if (vol <= 0) return;

    double dm = (vol == fr.volume) ? fr.margin : fr.margin * vol / fr.volume;
    double dc = (vol == fr.volume) ? fr.commission : fr.commission * vol / fr.volume;

    int left = vol;
    for (auto& [date, bvol] : fr.buckets) {
        if (left == 0) break;
        int take = std::min(left, bvol);
        if (take == 0) continue;
        auto it = positions_.find(posKey(fr.instrument, fr.pos_dir, date));
        if (it != positions_.end()) {
            auto& pos = it->second;
            if (fr.direction == Direction::Buy) pos.long_frozen = std::max(0, pos.long_frozen - take);
            else pos.short_frozen = std::max(0, pos.short_frozen - take);
            double share = static_cast<double>(take) / vol;
            pos.frozen_margin = std::max(0.0, pos.frozen_margin - dm * share);
            pos.frozen_commission = std::max(0.0, pos.frozen_commission - dc * share);
        }
        bvol -= take;
        left -= take;
    }

    account_.frozen_margin = std::max(0.0, account_.frozen_margin - dm);
    account_.frozen_commission = std::max(0.0, account_.frozen_commission - dc);
    fr.volume -= vol;
    fr.margin -= dm;
    fr.commission -= dc;
}

void PositionManager::releaseOrder(const std::string& order_key, int volume) {
    std::lock_guard lock(mtx_);
    auto it = order_freezes_.find(order_key);
    if (it == order_freezes_.end()) return;
    releaseLocked(it->second, volume);
    if (it->second.volume <= 0) order_freezes_.erase(it);
    recalcAccount();
}

std::optional<OrderFreeze> PositionManager::getOrderFreeze(const std::string& order_key) const {
    std::lock_guard lock(mtx_);
    auto it = order_freezes_.find(order_key);
    if (it == order_freezes_.end()) return std::nullopt;
    return it->second;
}

// ============================================================
// Trade Fill
// ============================================================

void PositionManager::onTradeFill(const TradeRecord& trade, const std::string& order_key) {
    std::lock_guard lock(mtx_);

    if (!order_key.empty()) {
        auto fit = order_freezes_.find(order_key);
        if (fit != order_freezes_.end()) {
            releaseLocked(fit->second, trade.volume);
            if (fit->second.volume <= 0) order_freezes_.erase(fit);
        }
    }

    auto inst_it = instruments_.find(trade.instrument);
    double mult = (inst_it != instruments_.end()) ? inst_it->second.volume_multiple : 1.0;
    char pos_date_type = (inst_it != instruments_.end()) ? inst_it->second.position_date_type : '1';

    int vol = trade.volume;

    if (trade.offset == Offset::Open) {
        // ---- OPENING TRADE ----
        char dir = (trade.direction == Direction::Buy) ? '2' : '3';
        auto& pos = positions_[posKey(trade.instrument, dir, '1')];
        pos.instrument = trade.instrument;
        pos.posi_direction = dir;
        pos.position_date = '1';

        double comm = calcCommissionLocked(trade.instrument, trade.price, vol, true, false);
        pos.commission += comm;
        account_.commission += comm;

        pos.position += vol;
        pos.today_position += vol;
        pos.open_cost += trade.price * vol * mult;
        pos.position_cost += trade.price * vol * mult;

        // calcMarginLocked resolves effective price via MarginPriceType:
        //   type='4' → trade.price (开仓价)
        //   type='1' → PreSettlementPrice (昨结算价)
        //   type='2' → LastPrice (最新价)
        double used_margin = calcMarginLocked(trade.instrument, trade.price, vol, dir, false);
        pos.margin += used_margin;
        account_.curr_margin += used_margin;
    } else {
        // ---- CLOSING TRADE ----
        char opp_dir = (trade.direction == Direction::Buy) ? '3' : '2';
        int remaining = vol;

        // 平仓顺序因交易所而异:
        //   SHFE/INE: 用户指定平今(CloseToday)或平昨(CloseYesterday), 各自内部FIFO
        //   DCE/CZCE: 先开先平, 优先平昨 (平今手续费减免时优先平今)
        //   CFFEX: 先开先平, 优先平昨; 但手续费按"优先平今"计算 (陷阱!)
        bool is_shfe = (inst_it != instruments_.end() &&
                        (inst_it->second.isSHFE() || inst_it->second.isINE()));
        bool is_cffex = (inst_it != instruments_.end() && inst_it->second.isCFFEX());

        std::vector<char> date_order = closeDateOrder(trade.instrument, trade.offset);

        // CFFEX commission trap tracking: fees calculated as if "today-first"
        int cffex_today_fee_remaining = 0;
        if (is_cffex) {
            // Find total today position to determine how many "today fee slots" exist
            auto today_key = posKey(trade.instrument, opp_dir, '1');
            auto today_it = positions_.find(today_key);
            if (today_it != positions_.end()) {
                cffex_today_fee_remaining = today_it->second.position;
            }
        }

        for (char d : date_order) {
            if (remaining <= 0) break;
            auto key = posKey(trade.instrument, opp_dir, d);
            auto it = positions_.find(key);
            if (it == positions_.end()) continue;
            auto& pos = it->second;

            int close_vol = std::min(remaining, pos.position);
            remaining -= close_vol;

            // Commission — split 平今/平昨 for exchanges that distinguish rates
            //   SHFE/INE: user specifies CloseToday/CloseYesterday, each slot has uniform rate
            //   CFFEX: close order = 优先平昨, commission = 优先平今 (trap, handled separately)
            //   DCE/CZCE/GFEX: 优先平昨, 平今 has fee discount (close_today rate may be ~0)
            double comm = 0;
            {
                if (is_cffex) {
                    int fee_as_today = std::min(close_vol, cffex_today_fee_remaining);
                    cffex_today_fee_remaining -= fee_as_today;
                    double comm_today = fee_as_today > 0
                        ? calcCommissionLocked(trade.instrument, trade.price, fee_as_today, false, true) : 0.0;
                    double comm_yesterday = (close_vol - fee_as_today) > 0
                        ? calcCommissionLocked(trade.instrument, trade.price, close_vol - fee_as_today, false, false) : 0.0;
                    comm = comm_today + comm_yesterday;
                } else if (is_shfe || pos_date_type == '2') {
                    // SHFE/INE: date slot determines fee type
                    bool isCT = (d == '1');
                    comm = calcCommissionLocked(trade.instrument, trade.price, close_vol, false, isCT);
                } else {
                    // DCE/CZCE/GFEX: single slot contains both today+yesterday positions
                    // Close order: yesterday first (date_order = {'2','1'}, but only d='1' exists)
                    int yd_pos = pos.position - pos.today_position;
                    int yesterday_vol = std::min(close_vol, yd_pos);
                    int today_vol = close_vol - yesterday_vol;
                    double comm_yd = yesterday_vol > 0
                        ? calcCommissionLocked(trade.instrument, trade.price, yesterday_vol, false, false) : 0.0;
                    double comm_td = today_vol > 0
                        ? calcCommissionLocked(trade.instrument, trade.price, today_vol, false, true) : 0.0;
                    comm = comm_yd + comm_td;
                }
                pos.commission += comm;
                account_.commission += comm;
            }

            // Close profit (mark-to-market)
            //   平昨 (d='2' or yesterday portion in DCE/CZCE): use settlement price
            //   平今 (d='1' or today portion in DCE/CZCE): use avg_open price
            double settle_px = 0.0;
            auto settle_it = pre_settlement_.find(trade.instrument);
            if (settle_it != pre_settlement_.end()) settle_px = settle_it->second;

            double old_close_profit = pos.close_profit;

            if (is_shfe || pos_date_type == '2') {
                // SHFE/INE: date slot determines which price to use
                if (d == '2') {
                    if (opp_dir == '2') {
                        pos.close_profit += (trade.price - settle_px) * close_vol * mult;
                    } else {
                        pos.close_profit += (settle_px - trade.price) * close_vol * mult;
                    }
                } else {
                    double avg_open = pos.position > 0 ? pos.open_cost / (pos.position * mult) : trade.price;
                    if (opp_dir == '2') {
                        pos.close_profit += (trade.price - avg_open) * close_vol * mult;
                    } else {
                        pos.close_profit += (avg_open - trade.price) * close_vol * mult;
                    }
                }
            } else {
                // DCE/CZCE/CFFEX/GFEX: split yesterday (settlement) vs today (avg_open)
                int yd_pos = pos.position - pos.today_position;
                int yesterday_vol = std::min(close_vol, yd_pos);
                int today_vol = close_vol - yesterday_vol;

                double avg_open = pos.position > 0 ? pos.open_cost / (pos.position * mult) : trade.price;
                if (opp_dir == '2') {
                    if (yesterday_vol > 0) pos.close_profit += (trade.price - settle_px) * yesterday_vol * mult;
                    if (today_vol > 0)     pos.close_profit += (trade.price - avg_open) * today_vol * mult;
                } else {
                    if (yesterday_vol > 0) pos.close_profit += (settle_px - trade.price) * yesterday_vol * mult;
                    if (today_vol > 0)     pos.close_profit += (avg_open - trade.price) * today_vol * mult;
                }
            }
            double close_profit_delta = pos.close_profit - old_close_profit;

            account_.close_profit += close_profit_delta;

            // Release margin proportionally (avoids open_price vs close_price mismatch
            // when MarginPriceType='4'—open margin used order price, not current price)
            double margin_to_release = (pos.position > 0)
                ? pos.margin * static_cast<double>(close_vol) / static_cast<double>(pos.position)
                : 0.0;
            pos.margin = std::max(0.0, pos.margin - margin_to_release);
            account_.curr_margin = std::max(0.0, account_.curr_margin - margin_to_release);

            // Update position
            pos.position -= close_vol;
            pos.position_cost = std::max(0.0, pos.position_cost - (trade.price * close_vol * mult));

        }
    }

    // Recalculate position_profit for all positions to remove closed ones
    // (closed positions have position=0 → position_profit must be 0 to avoid
    //  double-counting with close_profit in Balance)
    {
        double total_pos_profit = 0;
        for (auto& [key, pos] : positions_) {
            if (pos.position <= 0) {
                pos.position_profit = 0;
            } else {
                total_pos_profit += pos.position_profit;
            }
        }
        account_.position_profit = total_pos_profit;
    }

    recalcAccount();
}

// ============================================================
// Tick Update: Position Profit
// ============================================================

void PositionManager::onTick(const TickData& tick) {
    std::lock_guard lock(mtx_);  // exclusive lock for write

    // Filter CTP invalid value (double max = 1.7976931348623157e+308)
    // This value means the field is not applicable (e.g., SettlementPrice intraday)
    static constexpr double kCtpInvalid = 1.0e308;
    double last_px = tick.last_price;
    if (last_px >= kCtpInvalid) return;  // Skip invalid ticks (e.g., combo contract)

    last_price_[tick.instrument] = last_px;
    if (tick.pre_settlement > 0 && tick.pre_settlement < kCtpInvalid) {
        pre_settlement_[tick.instrument] = tick.pre_settlement;
    }
    if (tick.upper_limit > 0 && tick.upper_limit < kCtpInvalid) upper_limit_[tick.instrument] = tick.upper_limit;
    if (tick.lower_limit > 0 && tick.lower_limit < kCtpInvalid) lower_limit_[tick.instrument] = tick.lower_limit;

    // Note on AveragePrice/Turnover exchange differences (for K-line synthesis):
    //   郑商所(CZCE): AveragePrice from exchange, can use directly.
    //                 Turnover is CTP-calculated, may decrease between snapshots.
    //   其他交易所: AveragePrice = Turnover/Volume, need to divide by VolumeMultiple
    //   This gateway uses LastPrice for P&L, so these fields are passed through as-is.

    // Recalculate position profit for this instrument
    auto inst_it = instruments_.find(tick.instrument);
    double mult = (inst_it != instruments_.end()) ? inst_it->second.volume_multiple : 1.0;
    bool is_option = (inst_it != instruments_.end() && inst_it->second.isOption());

    if (is_option) {
        // Options have NO PositionProfit — PnL reflected in权利金收支 (CashIn)
        recalcAccount();
        return;
    }

    // Recalculate position-level profit for this instrument
    for (char dir : {'2', '3'}) {
        for (char date : {'1', '2'}) {
            auto key = posKey(tick.instrument, dir, date);
            auto it = positions_.find(key);
            if (it == positions_.end() || it->second.position <= 0) continue;
            auto& pos = it->second;

            double avg_price = pos.position > 0 ? pos.position_cost / (pos.position * mult) : tick.last_price;

            if (dir == '2') {
                pos.position_profit = (tick.last_price - avg_price) * mult * pos.position;
            } else {
                pos.position_profit = (avg_price - tick.last_price) * mult * pos.position;
            }
        }
    }

    // Recalculate account-level position_profit from SUM of all positions
    {
        double total_pos_profit = 0;
        for (auto& [k, p] : positions_) {
            total_pos_profit += p.position_profit;
        }
        account_.position_profit = total_pos_profit;
    }

    // Recalculate margin for today positions if MarginPriceType='2' (最新价)
    //   type='1' (昨结算价): fixed within session, no recalc needed
    //   type='4' (开仓价): fixed per position, no recalc needed
    if (broker_margin_price_type_ == '2' || broker_margin_price_type_ == '3') {
        double margin_delta = 0;
        for (char dir : {'2', '3'}) {
            for (char date : {'1', '2'}) {
                auto key = posKey(tick.instrument, dir, date);
                auto pit = positions_.find(key);
                if (pit == positions_.end() || pit->second.position <= 0) continue;
                auto& pos = pit->second;
                bool isYesterday = (date == '2');
                double new_margin = calcMarginLocked(tick.instrument, tick.last_price,
                                               pos.position, dir, isYesterday);
                margin_delta += new_margin - pos.margin;
                pos.margin = new_margin;
            }
        }
        account_.curr_margin += margin_delta;
    }
    recalcAccount();
}

// ============================================================
// Bulk Query
// ============================================================

void PositionManager::clearPositions() {
    std::lock_guard lock(mtx_);
    positions_.clear();
}

void PositionManager::onQueryPosition(const PositionRecord& pos) {
    std::lock_guard lock(mtx_);
    positions_[posKey(pos.instrument, pos.posi_direction, pos.position_date)] = pos;
}

void PositionManager::onQueryAccount(const AccountRecord& acc) {
    std::lock_guard lock(mtx_);
    account_ = acc;
    account_loaded_ = true;
}

// ============================================================
// Query
// ============================================================

std::optional<PositionRecord> PositionManager::getPosition(const std::string& inst, char dir,
                                                           char date) const {
    std::lock_guard lock(mtx_);
    auto it = positions_.find(posKey(inst, dir, date));
    if (it == positions_.end()) return std::nullopt;
    return it->second;
}

AccountRecord PositionManager::getAccount() const {
    std::lock_guard lock(mtx_);
    return account_;
}

double PositionManager::getAvailable() const {
    std::lock_guard lock(mtx_);
    return account_.available;
}

bool PositionManager::isAccountLoaded() const {
    std::lock_guard lock(mtx_);
    return account_loaded_;
}

void PositionManager::getAllPositions(std::vector<PositionRecord>& out) const {
    std::lock_guard lock(mtx_);
    for (const auto& [k, v] : positions_) {
        out.push_back(v);
    }
}

void PositionManager::reset() {
    std::lock_guard lock(mtx_);
    positions_.clear();
    order_freezes_.clear();
    account_ = AccountRecord{};
    account_loaded_ = false;
}

} // namespace falcon
