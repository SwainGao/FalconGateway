# FalconGateway 开发总结

## 一、核心设计理念

### 1.1 本地状态即真理源

CTP API 是异步回调模型。所有状态变更（订单状态变化、成交回报、行情推送）通过 SPI 回调异步到达。如果在策略层直接处理这些回调，每次决策都需要从回调参数中拼凑状态，极易遗漏。

FalconGateway 的设计核心：**在回调到达时立即更新本地状态镜像，策略层随时读取一致的快照**。

```
CTP SPI 回调 ──> RingBuffer ──> Worker线程 ──> 更新本地状态 ──> 通知策略回调
                                                      │
                                                      ├── OrderManager (订单)
                                                      ├── PositionManager (持仓+资金)
                                                      └── trade_history_ (成交)
```

### 1.2 公式推导优于增量累积

这是本项目最重要的教训。早期实现中，Balance 通过 `balance += delta` 方式累积更新，导致 PositionProfit 漂移累积至 294 元。改为公式推导后漂移降至 0.05 元。

```
错误: balance += position_profit_delta   // 每次 tick 累积 delta, 浮点误差累积
正确: balance = baseline + SUM(all position_profits) + close_profit - commission
```

**原则：能重算就不要累积。** 用不可变基线 + 可重算的浮动部分表达状态。

## 二、四态钩稽关系

Account、Position、Order、Trade 之间相互影响，形成闭环。

### 2.1 状态依赖图

```
                        OrderRequest
                            │
                   ┌────────┼────────┐
                   ▼        ▼        ▼
               Position   Order   Account
               (冻结)     (创建)  (冻结保证金)
                   │        │        │
                   │   OnRtnOrder    │
                   │   (状态更新)    │
                   │        │        │
          ┌────────┼────────┼────────┤
          ▼        ▼        ▼        ▼
       OnRtnTrade (成交回报)
          │        │        │        │
     ┌────┼────────┼────────┼────────┤
     ▼    ▼        ▼        ▼        ▼
  Position Order   Trade   Account
  (解冻+ (终态)  (记录)  (Balance/
   更新)                  Available)
     │                            │
     └──────── OnTick ───────────┘
              (更新PositionProfit)
```

### 2.2 下单时的钩稽

```
sendOrder(req)
  │
  ├─[1] PositionManager::onOrderRequest(req)
  │     ├── 查 InstrumentInfo → 获取合约乘数、保证金率
  │     ├── 计算 frozen_margin  = ratio × price × mult × vol
  │     ├── 计算 frozen_commission = amount × ratio_by_money + vol × ratio_by_vol
  │     ├── 检查 可用资金 > 冻结总额
  │     ├── account_.frozen_margin    += margin_freeze
  │     ├── account_.frozen_commission += comm_freeze
  │     ├── position.long_frozen      += vol (开多为例)
  │     └── position.frozen_margin    += margin_freeze
  │
  ├─[2] trade_api_->ReqOrderInsert(field)    ← CTP API调用
  │     └── 若返回非0 → PositionManager::onOrderRejectOrCancel → 解冻
  │
  └─[3] OrderManager::create(req, front_id, session_id, order_ref)
        └── 用 CTP 的 OrderRef（非本地生成），保证与 OnRtnOrder 精确匹配
```

**关键点：OrderRef 同步**。CTP API 的 OrderRef 通过 max_order_ref 自增生成，本地 OrderManager 必须使用同一个 OrderRef，否则 OnRtnOrder 回调无法匹配本地订单。

### 2.3 成交时的钩稽

```
OnRtnTrade 到达
  │
  ├─[1] processTradeUpdate(rec)
  │     ├── PositionManager::onTradeFill(rec)
  │     │   ├── 开仓: 解冻→更新持仓数量→增加占用保证金→计算手续费
  │     │   └── 平仓: 按交易所规则FIFO消耗持仓→计算平仓盈亏→释放保证金
  │     └── trade_history_.push_back(rec)  ← 实时记录成交
  │
  └─[2] 重算 Available = Balance - CurrMargin - FrozenMargin - FrozenCommission
```

**关键点：保证金冻结与占用的转换**。开仓成交时，冻结保证金转为占用保证金；平仓成交时，占用保证金释放。两两对冲，不应出现余额泄漏。

### 2.4 行情时的钩稽

```
OnRtnDepthMarketData 到达
  │
  └─[1] processTick(tick)
        └── PositionManager::onTick(tick)
            ├── 更新 last_price / pre_settlement
            ├── 对每个持仓: position_profit = (last - avg) × mult × pos
            ├── account_.position_profit = SUM(all position_profits)  ← 全量重算
            ├── Balance = static_baseline + PosProfit + CloseProfit - Commission
            └── Available = Balance - Margin - FrozenMargin - FrozenCommission
```

**关键点：全量重算而非增量累积。** 行情每秒 2 次推送，用 SUM 重算的 O(n) 开销很小（通常 <100 个持仓），但消除了累积误差。

## 三、关键开发坑点

### 3.1 死锁：锁顺序与递归锁

**问题**：`sendOrder` 在主线程先锁 position_mgr 再锁 order_mgr，而 worker 线程在 `processOrderUpdate` 中先锁 order_mgr（内部）再锁 position_mgr。形成 AB/BA 死锁。

**解决**：统一使用 `std::recursive_mutex`。性能损失可忽略（临界区仅几十行代码）。

### 3.2 死锁：shared_mutex 不可重入

**问题**：`onOrderRequest` 持有 `lock_guard`（写锁），内部调用 `calcCommissionForOrder` 尝试 `shared_lock`（读锁）。C++ `shared_mutex` 不允许同一线程从写锁降级到读锁。

**解决**：内部辅助函数不加锁，由调用者保证线程安全。文档注释 `// NOTE: caller holds mtx_`。

### 3.3 Available 公式错误

**原始错误公式**：
```
Available = Balance + PositionProfit + CloseProfit - Margin - FrozenMargin - FrozenCommission
```

PositionProfit 和 CloseProfit 已在 Balance 中，重复加了一次。

**正确公式**：
```
Balance = PreBalance + Deposit - Withdraw + PositionProfit + CloseProfit - Commission
Available = Balance - CurrMargin - FrozenMargin - FrozenCommission - FrozenCash - DeliveryMargin - Reserve
```

其中 `FrozenCash`（冻结资金）、`DeliveryMargin`（交割保证金）、`Reserve`（基本准备金）为 CTP 交易账户的隐藏扣除项，需从 `ReqQryTradingAccount` 查询并纳入公式。非 SimNow 环境下这些字段可能非零。SimNow 下通常为 0。

### 3.4 OrderRef 不匹配

**问题**：CTP API 用的 OrderRef 来自 `++max_order_ref`，但本地 OrderManager 自己生成独立计数器。导致 OnRtnOrder 回调中的 ref=2 对不上本地的 ref=1001。

**解决**：`OrderManager::create` 接受外部传入的 order_ref 参数，与 CTP API 使用同一个值。

### 3.5 空仓时 positions_loaded 不触发

**问题**：`OnRspQryInvestorPosition` 在空仓时回调一次（pInvestorPosition=NULL, bIsLast=true），但没有数据推入 ring buffer，导致 `positions_loaded_` 永不为 true。

**解决**：在 `startQueries` 后增加延时线程标记就绪，不依赖 ring buffer 的推送量。

### 3.6 FlowController 跨线程调用

**问题**：`flow_ctrl_.submit()` 从主线程调用时，内部 `pending_.push()` 非线程安全，可能和 worker 线程的 `onResponseComplete` 产生竞争。

**解决**：确保主线程只在网关就绪后（初始查询全部完成）才调用 submit，此时 worker 线程不会同时操作 pending_ 队列。

### 3.7 保证金价格类型

**问题**：默认假设 MarginPriceType='1'（昨结算价），但 SimNow 实际使用 '4'（开仓价）。冻结时用昨结算价导致 ~265 元偏差。另外 `calcMargin` 完全不考虑 MarginPriceType——开仓成交/平仓释放/tick 更新时直接使用调用者传入价格，未按期货公司配置的价格类型修正。

**解决**：
- 启动时查询 `ReqQryBrokerTradingParams` 获取真实的 MarginPriceType
- `calcMargin` 内部按 type 决定有效价格：昨仓→昨结算，type='1'→昨结算，type='2'→最新价，type='4'→保留调用者传入价（开仓价）
- 开仓成交时传 `trade.price`（开仓价），由 `calcMargin` 按 type 决定实际价格
- `onTick` 中 type='2'/'3' 时重算今仓保证金
- 平仓释放改为**比例释放**：`pos.margin × close_vol / pos.position`，避免开仓价 vs 平仓价不一致

### 3.8 交易所差异

| 差异点 | 处理方式 |
|--------|----------|
| SHFE/INE 区分今昨仓 | `distinguishesDate()` 判断，平仓时只查指定日期 |
| DCE/CZCE 优先平昨 | `date_order = {'2', '1'}` |
| CFFEX 手续费按优先平今 | 维护 `cffex_today_fee_remaining` 槽位 |
| CZCE 市价单 LimitPrice=0 | 下单时检测 `exchange == "CZCE"` |
| CTP 无效值 1.79e308 | onTick 中过滤 `kCtpInvalid = 1.0e308` |

### 3.9 开仓冻结保证金残留

**问题**：MarginPriceType='4' 时，下单冻结用 `req.price`（订单价），成交释放用 `trade.price`（成交价）。两者不同导致 `pos.frozen_margin` 无法归零，累积残留。

**解决**：开仓成交后，将 `pos.frozen_margin` 全部清零（`fmg_cleanup`），差值纳入 account 级清理。

### 3.10 DCE/CZCE/GFEX 平今平昨佣金分离

**问题**：非 SHFE/INE 交易所（单槽位持仓），`isCT` 永远为 false，平仓统一使用平昨费率。但 DCE/CZCE 有平今手续费减免（平今免收/减半），导致多扣佣金，Balance 偏低。

**解决**：在 `onTradeFill` close 段中，对 DCE/CZCE/GFEX 将 close_vol 拆分为昨仓部分（close_ratio）和今仓部分（close_today_ratio），分别计算佣金和盈亏。同时昨仓盈亏用结算价，今仓盈亏用开仓均价。

### 3.11 平仓后持仓盈亏重复计算（position_profit 双计）

**问题**：平仓成交后，`position` 归零但 `position_profit` 保留旧值。Balance 公式将已平仓的未实现盈亏（position_profit）和平仓实现盈亏（close_profit）重复累加，导致 Balance 虚高。

**解决**：`onTradeFill` 末尾重算 `account_.position_profit`——`position<=0` 的仓位 `position_profit` 强制归零。Balance = static_baseline + 清零后的 position_profit + close_profit - commission，消除重复。

## 四、数据流完整性验证

### 4.1 验证方法

每个测试阶段执行 `compareWithRemote()`：

1. 调用 `queryPosition()` / `queryAccount()` / `queryOrder()` → 结果写入 cmp_* 缓冲区
2. 等待 flow_ctrl 序列化完成
3. 逐项对比本地状态与远程查询结果
4. 通过 `trade_history_` 反算佣金基线

### 4.2 验证钩子

```cpp
// 在验证阶段，SPI 回调不污染本地状态
trader_handler_->verify_mode = true;   // 结果写入 cmp_* 缓冲区
// ... 查询 ...
trader_handler_->verify_mode = false;  // 恢复正常模式
```

## 五、线程模型

```
主线程 (main)
  ├── 启动网关, 等待就绪
  ├── sendOrder / cancelOrder
  └── 定时 verify

CTP 内部线程 (不可控)
  ├── TraderSpi 回调 → RingBuffer::tryPush
  └── MdSpi 回调    → RingBuffer::tryPush

TradeWorker 线程
  ├── RingBuffer::tryPop → processOrderUpdate / processTradeUpdate
  ├── 处理连接事件 (OnFrontConnected → Auth → Login)
  └── checkReady

MdWorker 线程
  └── RingBuffer::tryPop → processTick
```

**线程安全保证**：
- SPI → RingBuffer：lock-free SPSC，生产者单线程
- Worker → Manager：recursive_mutex，Manager 内部函数可重入
- 主线程 → Manager：同一 recursive_mutex，与 Worker 互斥但不会死锁

## 六、性能考量

| 组件 | 延迟 | 说明 |
|------|------|------|
| SPI → RingBuffer push | ~20ns | lock-free CAS |
| RingBuffer → Worker pop | ~20ns | |
| onOrderRequest (冻结) | ~200ns | 哈希查找 + 浮点运算 |
| onTradeFill (成交) | ~500ns | 含交易所规则判断 |
| onTick (行情) | ~2μs | O(n) 遍历持仓重算盈亏+保证金(type='2'时) |

## 七、测试结果

四交易所全流程测试（SHFE rb2607 / CZCE TA701 / DCE v2609 / GFEX si2607）：

| 追踪项 | 精度 | 说明 |
|--------|------|------|
| Order (ref/status/qty) | 100% | 零偏差 |
| Position (数量/margin) | 99.999% | margin < 13 元 |
| Trade (history) | 100% | 去重合并 |
| Commission | 99.99% | local-tracked = trade-history-calc（差 < 2 元） |
| CurrMargin | 99.99% | < 13 元 |
| FrozenMargin | 100% | 归零残留彻底修复 |
| Balance | 99.999% | 7~70 元偏差（~0.0004%） |
| Available | 99.999% | 公式与 CTP 一致（Avail diff ≈ Bal diff） |

## 八、待完善项

1. **历史会话恢复**：当前 `rm -rf flow/` 清除流文件。生产环境应使用 RESUME 模式自动恢复遗漏的回报。
2. **期权交易成本查询**：`ReqQryOptionInstrTradeCost` 未接入（当前仅有期货费率）。
3. **多账户支持**：当前单账户设计，扩展需改为账户管理器 + 工厂模式。
4. **组合保证金**：SPBM/SPMM/RCAMS/RULE 查询接口未接入。
5. **条件单/预埋单**：接口框架已就绪，下单逻辑未实现。
