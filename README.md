# FalconGateway

基于 CTP v6.7.2 的高性能 C++ 期货交易网关，提供完整的本地状态实时追踪能力。

## 特性

- **零拷贝 SPI 桥接**：Lock-free SPSC Ring Buffer，SPI 回调不阻塞，延迟 ~ns 级
- **完整本地状态维护**：实时追踪 Account / Position / Order / Trade，下单/撤单/成交时同步更新
- **远程校准**：启动时从 CTP 查询全量数据校准本地状态
- **精确保证金计算**：支持期货/期权，五种交易所差异（今昨仓、平仓顺序、手续费陷阱）
- **异步日志**：基于 Quill v11.1.0，TSC 时间戳，崩溃安全
- **公式化资金模型**：Balance 从静态权益基线推导，消除 PositionProfit 累积漂移

## 架构

```
strategy/          ← 用户策略代码（注册回调）
    │
FalconGateway      ← 主控网关（生命周期、查询、验证）
    │
├── TraderHandler  ← 交易 SPI（CTP线程 → RingBuffer → 工作线程）
├── MdHandler      ← 行情 SPI（Tick → RingBuffer → 工作线程）
├── OrderManager   ← 订单生命周期（三组序号：OrderRef/OrderSysID 双索引）
├── PositionManager← 持仓+资金（递归锁安全，公式化 Balance）
├── FlowController ← 查询流控（序列化，防 CTP -2/-3 错误）
└── RingBuffer     ← Lock-free SPSC 队列（65536 容量）
```

## 构建

```bash
# 前置要求: CMake 3.24+, C++17, Linux x86_64

cd Falcon_gateway
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles"
make -j4
```

产出：
- `libfalcon_gateway.a` — 静态库
- `falcon_main` — 示例/测试入口
- `liblogger.so` — 异步日志库

## 快速开始

```cpp
#include "gateway.h"
#include "logger.h"

int main() {
    logger::init();

    falcon::GatewayConfig cfg;
    cfg.broker_id   = "9999";
    cfg.user_id     = "221500";
    cfg.password    = "password";
    cfg.app_id      = "simnow_client_test";
    cfg.auth_code   = "0000000000000000";
    cfg.trade_front = "tcp://182.254.243.31:40001";
    cfg.market_front = "tcp://182.254.243.31:40011";
    cfg.subscribe_list = {"rb2607"};
    cfg.flow_path_trade  = "./flow/trade/";
    cfg.flow_path_market = "./flow/market/";

    falcon::FalconGateway gw(cfg);

    gw.setLoginCallback([](bool ok, const std::string& msg) {
        LOG_INFO("Login: {}", msg);
    });
    gw.setTradeCallback([](const falcon::TradeRecord& trd) {
        LOG_INFO("Trade: {} px={:.2f} vol={}", trd.instrument, trd.price, trd.volume);
    });

    gw.start();

    // 等待就绪（自动加载账户、持仓、合约、费率、MarginPriceType、历史成交）
    while (!gw.isReady()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 下单
    falcon::OrderRequest req;
    req.instrument = "rb2607";
    req.exchange   = "SHFE";
    req.direction  = falcon::Direction::Buy;
    req.offset     = falcon::Offset::Open;
    req.price      = 3000.0;
    req.volume     = 1;
    gw.sendOrder(req);

    // 验仓
    gw.verifyLocalState();   // 输出本地状态
    gw.compareWithRemote();  // 对比远程（账户/持仓/订单/成交/手续费）

    gw.stop();
}
```

## 启动加载序列

网关启动后自动链式查询 6 类数据（通过 FlowController 序列化）：

| 步骤 | 查询 | 说明 |
|------|------|------|
| 1 | ReqQryTradingAccount | 资金（Balance/Available/Margin） |
| 2 | ReqQryInvestorPosition | 持仓（数量/今昨仓/保证金/盈亏） |
| 3 | ReqQryInstrument | 合约信息（乘数/保证金率/今昨仓类型） |
| 4 | ReqQryInstrumentCommissionRate | 手续费率（开/平/平今） |
| 5 | ReqQryBrokerTradingParams | 经纪商参数（MarginPriceType） |
| 6 | ReqQryTrade | 历史成交（用于佣金基线校准） |

## 本地状态维护

### 账户(Account)

```
static_baseline = PreBalance + Deposit - Withdraw
Balance  = static_baseline + PositionProfit + CloseProfit - Commission
Available = Balance - CurrMargin - FrozenMargin - FrozenCommission - FrozenCash - DeliveryMargin - Reserve
```

所有事件（下单、撤单、成交、行情）均触发 Available 重算，Balance 从公式推导，消除累积漂移。

### 持仓(Position)

| 交易所 | 今昨仓 | 平仓顺序 | 平仓指定 |
|--------|--------|----------|----------|
| SHFE/INE | 区分(4条) | 用户指定 | CloseToday/CloseYesterday |
| DCE/CZCE | 不区分(2条) | 优先平昨 | OF_Close |
| CFFEX | 不区分(2条) | 优先平昨（手续费按优先平今）* | OF_Close |

*CFFEX 陷阱：平仓顺序和手续费计算规则不同，已在 onTradeFill 中实现。

### 订单(Order)

三组序号双索引追踪：

```
FrontID + SessionID + OrderRef  → 初始定位（CTP 接受前）
ExchangeID + OrderSysID          → 后续追踪（交易所分配后）
```

### 成交(Trade)

实时成交自动加入 `trade_history_`，启动时从 CTP 查询历史成交去重合并。通过成交历史反算佣金基线：

```
commission_baseline = SUM(trade.price × trade.volume × multiplier × commission_rate)
```

## 保证金

- **期货**：`保证金 = 保证金率 × 价格 × 合约乘数 × 手数`
- **期权**：`保证金 = MAX(权利金 + FixedMargin, MiniMargin)`
- **SHFE/INE 期权**：MiniMargin 非 0，昨仓权利金 = max(昨收盘, 昨结算) × 乘数
- **MarginPriceType**：从 BrokerTradingParams 查询，支持 4 种类型

## 流控

| 层级 | 限制 | 解决 |
|------|------|------|
| API 动态库 | 同一时间 1 个在途查询，每秒 N 次 | FlowController 序列化 |
| CTP 后台 | 报撤单 InvestorID 级别，FTD 报文 Session 级别 | 文档说明 |

## 测试

```bash
./falcon_main
```

测试场景：
1. **跌停价挂单**：验证冻结 → 撤单 → 释放完整链路
2. **涨停价挂单**：验证立即成交 → 持仓更新 → 资金变动

每个阶段自动对比本地状态与 CTP 远程查询结果。

## 测试结果

| 追踪项 | 精度 | 说明 |
|--------|------|------|
| Order (ref/status/qty) | 100% | 零偏差 |
| Position (数量/margin) | 100% | MarginPriceType 匹配 |
| Trade (history) | 100% | 去重合并 |
| Commission | 100% | trade-history 校准 |
| Balance | 99.999% | < 0.05 偏差 |
| Available | 99.999% | 公式推导 |

## 目录结构

```
Falcon_gateway/
├── CMakeLists.txt
├── main.cpp                  # 示例/测试入口
├── README.md
├── include/
│   ├── gateway.h             # 主控网关
│   ├── trader_handler.h      # 交易 SPI
│   ├── md_handler.h          # 行情 SPI
│   ├── order_manager.h       # 订单管理
│   ├── position_manager.h    # 持仓资金管理
│   ├── flow_controller.h     # 查询流控
│   ├── ring_buffer.h         # Lock-free SPSC Ring Buffer
│   ├── types.h               # 数据结构+回调类型
│   └── ctp/                  # CTP v6.7.2 头文件
├── src/
│   ├── gateway.cpp
│   ├── trader_handler.cpp
│   ├── md_handler.cpp
│   ├── order_manager.cpp
│   └── position_manager.cpp
├── lib/
│   ├── thosttraderapi_se.so
│   ├── thostmduserapi_se.so
│   └── error.xml
└── logger/                   # Quill 异步日志
    ├── CMakeLists.txt
    ├── include/logger.h
    └── src/logger.cpp
```
