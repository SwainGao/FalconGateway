# FalconGateway 测试技能

> 覆盖全流程：架构 → 开发坑点 → 四交易所测试 → 本地/远程状态对比 → 结果判读

---

## 一、架构速览

```
CTP SPI 回调 → RingBuffer(lock-free SPSC) → Worker线程 → 更新本地四态 → 通知策略回调
                                                           ├── Account (资金)
                                                           ├── Position (持仓+保证金)
                                                           ├── Order (订单双索引: OrderRef + OrderSysID)
                                                           └── Trade (成交历史)
```

### 启动加载链（6 步序列化，FlowController 排队）

| 步骤 | 查询 | 加载到 |
|------|------|--------|
| 1 | `ReqQryTradingAccount` | `PositionManager::account_` |
| 2 | `ReqQryInvestorPosition` | `PositionManager::positions_` |
| 3 | `ReqQryInstrument` | `PositionManager::instruments_`（含乘数/保证金率/今昨仓类型） |
| 4 | `ReqQryInstrumentCommissionRate` | `PositionManager::commissions_`（开/平/平今 4 个费率） |
| 5 | `ReqQryBrokerTradingParams` | `broker_margin_price_type_`（'1'=昨结算, '4'=开仓价） |
| 6 | `ReqQryTrade` | `trade_history_`（去重合并，用于佣金基线校准） |

---

## 二、开发关键坑点（8 个已验证）

### 1. 死锁：shared_mutex 不可重入
**现象**：`Resource deadlock avoided`  
**原因**：`lock_guard`（写锁）内部调 `calcCommissionForOrder` → `shared_lock`（读锁），同一线程不允许写→读降级  
**解决**：内部辅助函数不加锁，注释 `// NOTE: caller holds mtx_`

### 2. 死锁：锁顺序
**现象**：主线程 `sendOrder` 先锁 pos_mgr 再锁 order_mgr，worker 线程 `processOrderUpdate` 先锁 order_mgr 再锁 pos_mgr → AB/BA  
**解决**：order_mgr 和 position_mgr 均改用 `std::recursive_mutex`

### 3. Available 公式错误
**错误公式**：`Available = Balance + PositionProfit + CloseProfit - Margin - ...`（PositionProfit/CloseProfit 已在 Balance 中，重复加了）  
**正确公式**：
```
Available = Balance - CurrMargin - FrozenMargin - FrozenCommission - FrozenCash - DeliveryMargin - Reserve
Balance   = PreBalance + Deposit - Withdraw + PositionProfit + CloseProfit - Commission
```

### 4. Balance 累积漂移
**错误做法**：`balance += position_profit_delta`（浮点误差累积）  
**正确做法**：`balance = static_baseline + SUM(all position_profits) + close_profit - commission`（全量重算）

### 5. OrderRef 同步
**问题**：CTP API 用 `++max_order_ref`，本地 OrderManager 独立计数 → OnRtnOrder 无法匹配  
**解决**：`OrderManager::create` 接受外部传入的 order_ref，与 CTP API 共用同一值

### 6. 空仓时 positions_loaded 不触发
**问题**：空仓回调 `pInvestorPosition=NULL, bIsLast=true`，无数据推入 ring buffer  
**解决**：`startQueries` 后增加延时线程标记就绪

### 7. MarginPriceType 偏差
**问题**：默认假设 `'1'`（昨结算价），SimNow 实际用 `'4'`（开仓价），冻结偏差 ~265 元  
**解决**：启动时查询 `ReqQryBrokerTradingParams`，冻结时对 type='4' 使用订单价格

### 8. 开仓冻结/释放价差残留
**问题**：MarginPriceType='4' 时冻结用订单价（`req.price`），成交释放用成交价（`trade.price`），价差不一致 → `frozen_margin` 残留  
**解决**：成交后将该持仓的 `pos.frozen_margin` 全部清零（`fmg_cleanup`）

### 9. 保证金占用不按 MarginPriceType 更新
**问题**：`calcMargin` 直接使用调用者传入价格，不考虑 `broker_margin_price_type_`。开仓成交传 settle_px→trade.price，平仓释放用平仓价重算，tick 时不更新保证金  
**解决**：`calcMargin` 内部按 type 决定有效价格；平仓释放改为比例释放 `pos.margin × close_vol / pos.position`；`onTick` 中 type='2'/'3' 时重算今仓保证金

### 10. DCE/CZCE 平今平昨佣金未分离
**问题**：非 SHFE 交易所 `isCT` 永远 false，平仓统一用平昨费率。DCE/CZCE 平今免税但被多扣  
**解决**：DCE/CZCE/GFEX 平仓时拆分昨仓量和今仓量，分别使用 close_ratio 和 close_today_ratio

### 11. 平仓后持仓盈亏重复计算
**问题**：平仓后 `position=0` 但 `position_profit` 保留旧值。Balance = static + position_profit(含已平仓) + close_profit(平仓实现) → 重复计算  
**解决**：`onTradeFill` 末尾将 `position<=0` 的仓位 `position_profit` 强制归零

---

## 三、交易所差异处理

| 交易所 | 今昨仓 | 平仓顺序 | 平仓指定 | 手续费差异 |
|--------|--------|----------|----------|----------|
| SHFE / INE | 区分（4条 position 记录） | 用户指定 | CloseToday / CloseYesterday | 平今/平昨不同费率 |
| DCE | 不区分（2条，单槽 today+yd） | 优先平昨 | Offset::Close | 平今免收/减半 |
| CZCE | 不区分（2条） | 优先平昨 | Offset::Close | 平今免收/减半 |
| CFFEX | 不区分（2条） | 优先平昨 | Offset::Close | **手续费按优先平今计算**（陷阱） |
| GFEX | 不区分（2条） | 优先平昨 | Offset::Close | 标准费率 |

**平仓佣金分离**（`position_manager.cpp` 中 `onTradeFill` 的 close 段）：
- SHFE/INE：date='1' 用 `close_today_ratio`，date='2' 用 `close_ratio`
- DCE/CZCE/GFEX：昨仓部分用 `close_ratio`，今仓部分用 `close_today_ratio`（平今减免）
- CFFEX：close order 优先平昨，但佣金按优先平今计算（`cffex_today_fee_remaining` 槽位）

**平仓盈亏分离**：
- 昨仓：(平仓价 - 昨结算价) × 乘数 × 量
- 今仓：(平仓价 - 开仓均价) × 乘数 × 量

---

## 四、测试：四交易所全流程

### 4.1 测试合约选择

| 合约 | 交易所 | 特点 |
|------|--------|------|
| `rb2607` | SHFE | 今昨仓区分、平今平昨分开 |
| `TA701` | CZCE | 市价单 LimitPrice=0、Turnover 陷阱 |
| `v2609` | DCE | 优先平昨、平今手续费减免 |
| `si2607` | GFEX | 最新交易所、标准费率 |

**选择原则**：选当日有行情（`limits_ok` 能从 tick 捕获）、非临近交割月的活跃合约。

### 4.2 测试流程（每个交易所依次执行）

```
STEP0  → 基线对比（确认初始状态一致）
STEP1  → 涨停价买入开仓 → 立即成交 → 对比
STEP2  → 涨停价卖出平仓 → 价格太高无人买 → 挂单不成交 → 对比
STEP3  → 撤单 → 释放冻结 → 对比
STEP4  → 跌停价卖出平仓 → 便宜卖立即成交 → 持仓归零 → 对比
```

**价格逻辑说明**：

| 操作 | 价格 | 方向 | 为什么这样设计 |
|------|------|------|--------------|
| STEP1 开多 | `UpperLimit`（涨停） | Buy | 愿意高价买 → 市价≤涨停 → 立即成交 |
| STEP2 平多 | `UpperLimit`（涨停） | Sell | 想高价卖 → 市价<涨停无人买 → **不成交，挂单** |
| STEP3 撤单 | — | — | 验证撤单链路：冻结→释放 |
| STEP4 平多 | `LowerLimit`（跌停） | Sell | 愿意便宜卖 → 市价≥跌停 → **立即成交** |

> **关键**：涨停卖单 = 挂单（价格太高无人接）；跌停卖单 = 成交（便宜卖有人抢）。如果设计反了（STEP2 用跌停价），会立即成交达不到测试挂单的目的。

### 4.3 每个 STEP 后 `compareWithRemote()` 的对比逻辑

#### (A) Account 对比

```cpp
verify_mode = true  // SPI 回调重定向到 cmp_* 缓冲区，不污染本地状态

// 序列化查询
queryPosition()  → OnRspQryInvestorPosition → cmp_positions
queryAccount()   → OnRspQryTradingAccount   → cmp_accounts
queryOrder()     → OnRspQryOrder            → cmp_orders

// 等待 4 秒后读取，逐项对比
verify_mode = false
```

**对比项**：

| 字段 | 容差 | 来源 |
|------|------|------|
| Balance | < 0.02 | `pre_balance + deposit - withdraw + position_profit + close_profit - commission` |
| Available | < 0.02 | `Balance - CurrMargin - FrozenMargin - FrozenCommission - FrozenCash - DeliveryMargin - Reserve` |
| CurrMargin | < 0.02 | SUM(各持仓保证金) |
| FrozenMargin | < 0.02 | 待成交开仓冻结保证金 |
| FrozenCommission | — | 待成交冻结手续费 |
| FrozenCash | — | CTP 冻结资金 |
| DeliveryMargin | — | 交割保证金 |

#### (B) Position 对比

本地 `positions_` map（key = inst + direction + date）vs 远程 cmd_positions：

```
对比字段：position(总持仓), today_position(今仓), margin(保证金), long_frozen, short_frozen
容差：margin < 0.02, 数量精确匹配
```

检查双向：
- 远程有而本地无 → `ONLY-REMOTE`
- 本地有（非零仓位/冻结）而远程无 → `ONLY-LOCAL`

#### (C) Order 对比

本地 active orders vs 远程全部 orders。匹配方式：
1. `OrderRef`（FrontID + SessionID + OrderRef 拼接为 key）
2. 回退到 `OrderSysID`（交易所系统编号）

对比：status（状态字符）、volume_traded（已成交）、volume_total（总数量）

#### (D) Trade 对比

`trade_history_`（本地累积）vs `remote_trades_`（CTP 查询整个交易日），按 `trade_id` 去重匹配。

#### (E) Commission 核对

两种佣金计算交叉验证：
1. `local-tracked`：`PositionManager::account_.commission`（逐笔累加）
2. `trade-history-calc`：对 `trade_history_` 重算 SUM → 独立校验

---

## 五、每个 STEP 状态变化预期

### STEP 0：基线

| 状态 | 预期 |
|------|------|
| Account | Balance/Avail/Margin 与 CTP 完全一致 |
| Position | 方向+日期+数量+保证金完全一致 |
| Order | 本地无活跃单（已清空） |
| Trade | trade_history_ 已从 CTP 加载 |

### STEP 1：涨停买开（成交）

| 组件 | 本地状态变化 | 预期对比结果 |
|------|------------|------------|
| Position | 多头今仓 +1，margin += 开仓保证金 | pos=1(1) margin 匹配 |
| Account | FrozenMargin 释放→CurrMargin 增加，Commission 累加 | mgn diff < 0.02，fmg diff 0 |
| Order | ref=2 状态→已成交(0/'0') | status 一致 |
| Trade | trade_history_ push 一笔，remote_trades_ 包含 | 匹配 +1 |

### STEP 2：涨停卖平（挂单不成交）

| 组件 | 本地状态变化 | 预期对比结果 |
|------|------------|------------|
| Position | short_frozen +1（冻结平仓仓位），持仓不变 | short_frozen 一致 |
| Account | FrozenCommission += 平仓手续费（冻结） | FrzComm 差值为平仓费 |
| Order | ref=3 状态→NoTradeQueueing('3') | status 一致 |
| Trade | **无变化**（未成交） | match count 不变 |

### STEP 3：撤单

| 组件 | 本地状态变化 | 预期对比结果 |
|------|------------|------------|
| Position | short_frozen 归零，frozen_margin/commission 释放 | short_frozen=0 |
| Account | FrozenMargin/FrozenCommission 归零 | FrzMargin/FrzComm 归零 |
| Order | ref=3 状态→Canceled('5') | status=5 一致 |

### STEP 4：跌停卖平（成交，持仓归零）

| 组件 | 本地状态变化 | 预期对比结果 |
|------|------------|------------|
| Position | 多头仓位减至 0，margin 释放 | pos=0 margin=0 |
| Account | CurrMargin 减少，CloseProfit 增加，Commission 累加 | bal/avl 差值反映 P&L 差异 |
| Order | ref=4 状态→已成交(0/'0') | status 一致 |
| Trade | trade_history_ push 一笔 | 匹配 +1 |

---

## 六、保证金实时占用工作流

### 价格决定规则

```
今仓保证金价 = MarginPriceType == '1' ? PreSettlementPrice (昨结算, 当日不变)
             : MarginPriceType == '2' ? LastPrice (最新价, 每 tick 更新)
             : MarginPriceType == '4' ? 开仓价 (成交价, 持仓期间不变)
昨仓保证金价 = PreSettlementPrice (固定, 始终用昨结算)
```

### 更新时机

| 事件 | 保证金更新 | 说明 |
|------|----------|------|
| 开仓成交 | `pos.margin = calcMargin(成交价)` | type='4'→开仓价, type='1'→昨结算 |
| Tick(type='2') | `pos.margin = calcMargin(最新价)` | 今仓保证金随行情动态更新 |
| 平仓成交 | `pos.margin -= 比例释放` | `pos.margin × close_vol / pos.position` |
| Tick(type≠'2') | 不更新 | 昨结算/开仓价不变 |

### 平仓释放为什么用比例而非重算

```
开仓时: pos.margin = 3335(订单价) × 10 × 8% × 1 = 2668
平仓时(若重算): release = 3187(平仓价) × 10 × 8% × 1 = 2549.6
残留: 2668 - 2549.6 = 118.4 ← 价差导致 frozen_margin 无法归零

正确: release = pos.margin × close_vol/pos.position = 2668 × 1/1 = 2668 ← 完全释放
```

---

## 七、运行测试

### 构建

```bash
cd Falcon_gateway
make -C build -j4
```

### 运行（全新会话）

```bash
rm -rf flow           # 清除流文件，CTP 从零开始
timeout 180 ./build/falcon_main
```

### 过滤关键日志

```bash
./build/falcon_main 2>&1 | grep -E "Subscribed|limits|ACCOUNT:|MISMATCH|MATCH|Order sent|Trade fill|CancelAll|COMMISSION:|TEST COMPLETE|ALL TESTS"
```

---

## 七、结果判读

### 完美匹配（可预期）

```
[Compare] POSITION: local=3 remote=3
[Compare]   rb2607 dir=2 dt=1 OK pos=10(10) today=10(10) margin=50932.80(50932.80) lf=0(0) sf=0(0)
[Compare] ORDER: local-active=0 remote=60
[Compare]   rb2607 ref=3 OK status=5(5) filled=0/1(0/1)
[Compare] TRADE: 23 matched, 0 new-remote
```

Position 全部 `OK`，Order 全部 `OK`，Trade 全部 `matched` → 核心状态追踪 100% 准确。

### 可接受的差异

```
[Compare] ACCOUNT MISMATCH (bal:5.00 avl:5.00 mgn:0.00 fmg:0.00)
```

- **bal diff = avl diff**：公式一致，差异来自 PositionProfit/Commission 计算精度
- **bal 差 < 100 元**（相对 2000 万权益 < 0.0005%）：可接受
- **mgn diff = 0, fmg diff = 0**：保证金追踪完全正确

### 需要排查的异常

| 异常现象 | 可能原因 |
|----------|----------|
| fmg diff > 1.0 | 冻结/释放价差残留（检查 MarginPriceType） |
| ONLY-REMOTE position | 本地持仓未创建（检查 onQueryPosition 初始化） |
| ONLY-LOCAL position | 远程已平仓但本地未清理（检查 onTradeFill 平仓逻辑） |
| ONLY-REMOTE order | OrderRef 不匹配（检查 FrontID/SessionID/OrderRef 同步） |
| TRADE: N new-remote | 历史成交未加载（检查 ReqQryTrade 时机） |
| Commission local≠calc | 基线校准时机问题或费率查询缺失 |

### Balance diff 根因速查

| 差异级别 | 大概原因 |
|----------|----------|
| ~0 元 | Balance 公式+费率+均价完全一致 |
| < 10 元 | 浮点累计算术误差 |
| 10~100 元 | PositionProfit（LastPrice vs CTP 内部价）或 Commission 基线 |
| 100~300 元 | 开仓均价（position_cost 聚合 vs CTP 分笔 FIFO）+ 佣金差额 |
| > 300 元 | 大概率有 bug（Missing trade/commission/close_profit） |

---

## 八、代码关键文件

| 文件 | 职责 |
|------|------|
| `main.cpp` | 测试入口，四交易所 TestContext + runExchangeTest |
| `include/gateway.h` | 主控网关 API + 生命周期 |
| `src/gateway.cpp` | 启动序列、`compareWithRemote()`、trade 去重 |
| `include/position_manager.h` | 持仓+资金管理（recursive_mutex） |
| `src/position_manager.cpp` | `onOrderRequest`/`onTradeFill`/`onTick`、佣金/保证金/盈亏计算 |
| `include/trader_handler.h` | SPI 实现、verify_mode、cmp_* 缓冲区 |
| `include/order_manager.h` | 订单生命周期、OrderRef/OrderSysID 双索引 |
| `include/types.h` | 所有数据结构定义 |
| `DEV_SUMMARY.md` | 开发经验总结 |
| `README.md` | 项目架构文档 |
