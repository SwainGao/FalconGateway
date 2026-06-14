#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <chrono>
#include <functional>

namespace falcon {

using i64 = int64_t;
using i32 = int32_t;

// ============================================================
// Time
// ============================================================
using Timestamp = std::chrono::steady_clock::time_point;
inline Timestamp now() { return std::chrono::steady_clock::now(); }

// ============================================================
// Order
// ============================================================
enum class Direction : char { Buy = '0', Sell = '1' };
enum class Offset : char { Open = '0', Close = '1', CloseToday = '3', CloseYesterday = '4' };
enum class OrderStatus : char {
    AllTraded = '0', PartTraded = '1', NoTrade = '3', Canceled = '5', Unknown = 'a'
};
enum class OrderType : char { Limit = '0', Market = '1' };

struct OrderRequest {
    std::string instrument;
    std::string exchange;
    Direction direction;
    Offset offset;
    double price;
    int volume;
    OrderType type = OrderType::Limit;
    char hedge_flag = '1';  // 投机
    int request_id = 0;
};

struct OrderRecord {
    // 三组序号
    int front_id = 0;
    int session_id = 0;
    std::array<char, 21> order_ref{};
    std::array<char, 21> order_sys_id{};

    OrderRequest request;
    OrderStatus status = OrderStatus::Unknown;
    char submit_status = '3';  // Accepted

    int volume_traded = 0;
    int volume_total = 0;
    double traded_price = 0.0;

    Timestamp create_time;
    Timestamp update_time;
    bool is_local = true;  // 是否本session发出的

    [[nodiscard]] auto orderKey() const {
        return std::to_string(front_id) + "_" + std::to_string(session_id) + "_"
             + std::string(order_ref.data());
    }
    [[nodiscard]] auto exchangeKey() const {
        return std::string(order_sys_id.data());
    }
    [[nodiscard]] bool isFinal() const {
        return status == OrderStatus::AllTraded || status == OrderStatus::Canceled;
    }
};

// ============================================================
// Trade
// ============================================================
struct TradeRecord {
    std::array<char, 21> trade_id{};
    std::string instrument;
    std::string exchange;
    Direction direction;
    Offset offset;
    double price = 0.0;
    int volume = 0;
    std::array<char, 21> order_sys_id{};
    Timestamp trade_time;
};

// ============================================================
// Position
// ============================================================
struct PositionRecord {
    std::string instrument;
    char posi_direction = '2';   // '2'=多, '3'=空
    char position_date = '1';    // '1'=今, '2'=昨(仅SHFE/INE)
    char hedge_flag = '1';

    int position = 0;
    int today_position = 0;
    int yd_position = 0;          // 静态昨仓
    int long_frozen = 0;
    int short_frozen = 0;

    double position_cost = 0.0;
    double open_cost = 0.0;
    double margin = 0.0;
    double frozen_margin = 0.0;
    double frozen_commission = 0.0;
    double commission = 0.0;
    double close_profit = 0.0;
    double position_profit = 0.0;

    [[nodiscard]] int availClose() const {
        return posi_direction == '2'
            ? position - short_frozen   // 多头可平=总-空头冻结
            : position - long_frozen;    // 空头可平=总-多头冻结
    }
    [[nodiscard]] int ydActual() const {
        return position - today_position;
    }
};

// ============================================================
// Account
// ============================================================
struct AccountRecord {
    double balance = 0.0;         // 动态权益
    double available = 0.0;       // 可用资金
    double pre_balance = 0.0;     // 静态权益
    double curr_margin = 0.0;     // 占用保证金
    double frozen_margin = 0.0;
    double frozen_commission = 0.0;
    double frozen_cash = 0.0;     // 冻结资金 (FrozenCash)
    double delivery_margin = 0.0; // 交割保证金 (DeliveryMargin)
    double reserve = 0.0;         // 基本准备金 (Reserve)
    double close_profit = 0.0;
    double position_profit = 0.0;
    double commission = 0.0;
    double deposit = 0.0;
    double withdraw = 0.0;
};

// ============================================================
// Market Data (Tick)
// ============================================================
struct TickData {
    std::string instrument;
    std::string exchange;
    std::string update_time;     // "HH:MM:SS"
    int update_millisec = 0;

    double last_price = 0.0;
    double bid_price1 = 0.0;
    double ask_price1 = 0.0;
    int bid_volume1 = 0;
    int ask_volume1 = 0;
    double volume = 0.0;
    double turnover = 0.0;
    double open_interest = 0.0;
    double upper_limit = 0.0;
    double lower_limit = 0.0;
    double pre_close = 0.0;
    double pre_settlement = 0.0;
    double pre_open_interest = 0.0;

    Timestamp recv_time;
};

// ============================================================
// Callbacks (策略层接口)
// ============================================================
using TickCallback = std::function<void(const TickData&)>;
using OrderCallback = std::function<void(const OrderRecord&)>;
using TradeCallback = std::function<void(const TradeRecord&)>;
using PositionCallback = std::function<void(const PositionRecord&)>;
using AccountCallback = std::function<void(const AccountRecord&)>;
using ErrorCallback = std::function<void(int err_id, const std::string& err_msg)>;
using LoginCallback = std::function<void(bool success, const std::string& msg)>;
using ConnectedCallback = std::function<void()>;

// ============================================================
// Instrument Info (合约信息)
// ============================================================
struct InstrumentInfo {
    std::string product_id;       // 品种代码
    std::string exchange_id;      // 交易所代码
    char product_class = '1';     // '1'=期货 '2'=期权 '3'=组合
    double volume_multiple = 1.0; // 合约乘数
    double price_tick = 1.0;      // 最小变动价位
    double underlying_multiple = 0.0; // 期权基础商品乘数

    // 保证金率（交易所级别，实际计算需加期货公司上浮）
    double long_margin_ratio = 0.0;
    double short_margin_ratio = 0.0;
    char margin_price_type = '1';  // '1'=昨结算 '2'=最新价 '3'=均价 '4'=开仓价
    char position_date_type = '1'; // '1'=不分今昨 '2'=区分(仅SHFE/INE)
    char max_margin_side = '1';    // '1'=双边 '2'=大额单边

    // 期权专用
    double strike_price = 0.0;
    char options_type = '0';       // '1'=看涨 '2'=看跌
    std::string underlying_id;     // 标的合约

    // 期权保证金不变部分（从 ReqQryOptionInstrTradeCost 获取）
    double opt_fixed_margin = 0.0;   // FixedMargin（保证金不变部分）
    double opt_mini_margin = 0.0;    // MiniMargin（最小保证金，仅上期所非0）
    double opt_exch_fixed_margin = 0.0;   // 交易所级别
    double opt_exch_mini_margin = 0.0;

    // 交易限制
    int max_limit_order_volume = 0;
    int min_limit_order_volume = 1;
    int max_market_order_volume = 0;
    int min_market_order_volume = 1;

    char is_trading = '0';
    char inst_life_phase = '0';    // 合约生命周期

    [[nodiscard]] bool isOption() const { return product_class == '2'; }
    [[nodiscard]] bool isSHFE() const { return exchange_id == "SHFE"; }
    [[nodiscard]] bool isINE() const { return exchange_id == "INE"; }
    [[nodiscard]] bool isDCE() const { return exchange_id == "DCE"; }
    [[nodiscard]] bool isCZCE() const { return exchange_id == "CZCE"; }
    [[nodiscard]] bool isCFFEX() const { return exchange_id == "CFFEX"; }
    [[nodiscard]] bool distinguishesDate() const { return position_date_type == '2'; }
};

// ============================================================
// Commission Info (手续费信息)
// ============================================================
struct CommissionInfo {
    double open_ratio_by_money = 0.0;    // 开仓按金额费率
    double open_ratio_by_volume = 0.0;   // 开仓按手数费率
    double close_ratio_by_money = 0.0;   // 平仓按金额费率
    double close_ratio_by_volume = 0.0;  // 平仓按手数费率
    double close_today_ratio_by_money = 0.0;   // 平今按金额费率
    double close_today_ratio_by_volume = 0.0;  // 平今按手数费率
    double order_comm_by_volume = 0.0;         // 报单申报费（每笔）
    double order_action_comm_by_volume = 0.0;  // 撤单申报费（每笔）
};

// ============================================================
// Config
// ============================================================
struct GatewayConfig {
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;

    std::string trade_front;     // "tcp://ip:port"
    std::string market_front;

    std::string flow_path_trade = "./flow/trade/";
    std::string flow_path_market = "./flow/market/";

    // 订阅合约列表（为空则订阅全市场，需从交易API获取合约列表后订阅）
    std::vector<std::string> subscribe_list;

    // 合约查询品种过滤（空=全市场，填品种代码如"rb"则只查该品种）
    std::string product_filter;
};

} // namespace falcon
