# jarvistrader 实现计划

本计划按 [architecture.md](architecture.md) 拆分为 M0–M7 八个里程碑，覆盖全部模块。约定：

1. 每个任务以"模块 —"开头，模块名与 architecture.md 第 3 节的分层一致；`harness` 表示测试、基准或形式化验证任务，`验收` 是该里程碑的完成标准。
2. 每个任务合并前必须通过 architecture.md 第 17 节的门禁：功能测试 → 性能对比 → 触及核心路径时的形式化验证。
3. v1.0 在 M5 结束时发布。M6 为 v1.x，M7 为规模化。
4. 示例策略只放在 `examples/`，只供测试与 soak 使用，不进 wheel。

## M0 工程门与 harness 骨架（在写领域代码之前，让三层门禁都能运行）

> 2026-09-26 完成。验收结果：`just check` 通过（指纹门与 A/B 基准按设计输出 SKIPPED）；注入内核 include `<chrono>` 与 shell 层头文件，分层检查报 2 处违规；注入内核头 `throw`，freestanding 目标编译失败；在零分配门的计量范围内分配，测试报 1000 次分配；`noop/loop` 实测回归 +15.0%，三轮 A/B 中复现两轮，判定 REGRESSION 并非零退出；不改代码的对照组为 -1.7%，判定 ok。另修复了脚手架遗留问题：clang-tidy 的头文件过滤正则从未匹配绝对路径，项目头文件此前没有被检查。

- [x] task: build — 新增 `jarvis_shell` 静态库目标与 `JARVIS_BUILD_LIVE` 选项，纯回测 wheel 不依赖网络栈（architecture §3、§13）
- [x] task: build — 新增 `tsan` 与 `fuzz` 两个 CMake preset，并接入 justfile（§17.2）
- [x] task: tools — `tools/check-layering.py`：按 §3 分层表检查 include 关系，接入 pre-commit 与 CI
- [x] task: tests — 按层建立 `tests/cpp/test_<layer>.cpp` 骨架与 ctest 标签 `unit`、`property`、`conformance`、`golden`、`zero-alloc`（`jarvis_add_layer_test()`；各层的测试二进制随该层代码一起加入）
- [x] task: testkit — `jarvis::testkit::Gen`（splitmix64）性质测试生成器，支持 `JARVIS_PROP_SEED` 与 `JARVIS_PROP_ITERS`
- [x] task: tests — 零分配门夹具：debug 构建替换 `operator new` 并计数
- [x] task: tools — golden 工具骨架：`tests/golden/<case>/` 布局、`just golden`、`just golden-update`
- [x] task: bench — 建立 `benchmarks/hot/`、`benchmarks/report/`、`benchmarks/thresholds.toml`，用一个 `bench_noop` 基准验证管线
- [x] task: tools — `tools/bench_compare.py`：中位数比较、逐项阈值与 `abs_floor_ns`、三轮 A/B 中两轮复现才判定回归
- [x] task: ci — `bench-compare` job：`git worktree` 同时构建 merge-base 与 head，在同一 runner 上绑核交替运行
- [x] task: specs — `specs/tla/` 骨架、钉定 `tla2tools.jar` 的 SHA-256、一个 20 行的占位规约
- [x] task: tools — `specs/tla/MAP.toml`、`tools/tla/select_specs.py`、由两者生成 `tools/core-paths.txt`（§17.4、§17.5）
- [x] task: ci — `formal` job（仅当改动触及核心路径时运行）与 PR 的 `core` 标签自动标注
- [x] task: ci — 作业图改为 lint → functional → {determinism, bench-compare, formal} → gate，新增 nightly 工作流（§17.6）
- [x] task: justfile — `just check` 与 §17.7 的全部配方；对应功能落地前，配方明确打印"跳过：尚未实现"，不伪装为通过
- [x] task: docs — 修订 `docs/cpp-subset.md`：`OrderId` 更名为 `OrderHandle`，补充热路径定义、`DynamicStrategySet` 函数指针表例外、零分配规则（D06、D24）
- [x] task: docs — 更新 `init-project.md` 的非目标：不内置策略；v1.0 订单子集为 MARKET、LIMIT（GTC/IOC/FOK/GTX）、reduceOnly、STP
- [x] task: 验收 — 在当前脚手架上 `just check` 通过；分别故意引入一次分层违规、一次 `step` 内分配、一次 15% 基准回归，三道门各自报错

## M1 core 与 model（兼容契约落地，确定性门变成真实检查）

> 2026-09-26 完成。验收结果：GCC 13 Release、GCC 13 `-O0`（det-o0）与 Clang 18 Release 各自生成 seed 7 的 20 万条语料（覆盖全部 33 种记录类型），事件日志逐字节一致；Python 把语料读入后原样写出，指纹不变，说明每个字段在 C++ 与 Python 之间无损往返；`just check` 通过（lint、24 组 ctest、155 个 pytest、golden、指纹门、A/B 基准、TLC）。模糊测试（每个目标 60–90 秒）：decimal 约 1000 万次、wire 约 2300 万次无失败，wire 目标同时证明编码是规范的（能解码的记录重新编码后逐字节相同）；config 目标发现 toml++ v3.4.0 在值后出现 U+3002 等字符时执行到 `TOML_UNREACHABLE`（未定义行为），已改钉上游修复后的 master 提交并把输入加入回归语料。实现中的修正：与 nautilus 测试向量对照后，`Money` 与指定精度的解析改为 round half to even（原先报 PrecisionLoss）；CRC32C 使用硬件指令后，`log/append_record` 从 289 ns 降到 89 ns，`log/decode_record` 从 274 ns 降到 75 ns。计划外补充：`jarvis` 命令行（corpus、fingerprint、dump、roundtrip、config）、模型字段描述符 `jarvis/model/schema.hpp`（日志编码、文本输出与 Python 绑定共用）、PR 级 fuzz CI job。

- [x] task: core — `Status`、`FixedVector`（超容返回 `Status::CapacityExceeded`）、slab 竞技场与带代际的 32 位句柄
- [x] task: core — counter-based RNG（splitmix64、Philox），键为 `(seed, identity, hop)`，不依赖调用顺序
- [x] task: core — `UnixNanos`、`DurationNanos` 与 RFC 3339 格式化
- [x] task: core — 事件键 `(ts, source_id, seq)` 与确定性优先队列
- [x] task: core — `Clock` concept、`ReplayClock` 与定时器轮
- [x] task: model — `Price`、`Quantity`、`Money`、`Currency`：raw 按 1e9 刻度、字符串解析与格式化、`__int128` 乘法与向零截断（§6.1）
- [x] task: model — 全部标识符及其字符串约束，`InstrumentId` intern 为 `uint32` 槽位的侧表（§6.2）
- [x] task: model — 全部枚举，保留 nautilus 的整数值与字符串（§6.6）
- [x] task: model — 行情数据类型：`TradeTick`、`QuoteTick`、`Bar`、`BarType`、`BarSpecification`、`BookOrder`、`OrderBookDelta`、`OrderBookDeltas`、`OrderBookDepth`、`InstrumentStatus`、`MarkPriceUpdate`、`IndexPriceUpdate`、`FundingRateUpdate`、`InstrumentClose`，以及扩展类型 `LiquidationOrder`（§6.4）
- [x] task: model — Instrument：`CurrencyPair`、`CryptoPerpetual`、`CryptoFuture` 与校验规则（§6.5）
- [x] task: model — 17 种订单事件、仓位事件、`AccountState`、`AccountBalance`、`MarginBalance`（§6.7）
- [x] task: model — `ClientOrderId` 生成器 `{node_tag}-{epoch}-{seq}`（Base32）与解码，epoch 持久化计数器（§8.4）
- [x] task: model — 封闭的 `Event` variant 与事件分类（§5.1）
- [x] task: log — 定宽显式编码的事件日志：日志头、记录布局、crc32c、按段滚动（§5.6、§16.1）
- [x] task: tools — `jarvis fingerprint`，替换 CI determinism job 与 `just fp` 中的占位输出
- [x] task: build — 钉定 toml++ 的提交，供 shell 解析配置使用
- [x] task: config — 类型化 `NodeConfig`：TOML 解析（位于 shell）、未知键报错、`--env` 与 `--set` 覆盖、规范化后计算 hash（§4.2）
- [x] task: node — Node 生命周期状态机与 `NodeLifecycle` 记录事件，先实现 backtest 路径用到的状态（§4.4）
- [x] task: python — nanobind 绑定全部模型类型，跨边界一律按值拷贝
- [x] task: python — `jarvis.determinism.guard()` 与 `PYTHONHASHSEED` 自动设置（§7.7）
- [x] task: harness — 定点算术性质测试：字符串往返、比较只看 raw、乘法截断方向、溢出检测
- [x] task: harness — nautilus 字符串格式往返 golden：标识符、`Price`、`Quantity`、`Money`、`BarType`、枚举
- [x] task: harness — 与 nautilus `cd417b80` 源码逐项核对字段、枚举值、常量的核对测试
- [x] task: harness — 热基准 `model/parse_decimal`、`log/append_record`
- [x] task: 验收 — rel 与 det-o0 在生成的模型与事件语料上逐字节一致；Python 能构造全部模型类型并读写事件日志

## M2 Engine、data、策略宿主与回放（Python 回测端到端跑通，只有数据）

- [x] task: engine — `Engine<StrategySet>`、`EngineState`、`EventSource` 与 `CommandSink` concept（§3、§4.3）
- [x] task: data — `Router` 与 `SubscriptionMatrix`，类型化 `Subscription{ slot, kind, cadence }`（§7.2）
- [x] task: data — `Cadence`：`Every`、`Conflated`、`SampledNs`、`OnBatch`，以及 `BatchEnd` 记录事件（§7.5）
- [x] task: data — 订单簿 L1 与 L2：按 tick 索引的稠密价位表示，只读的 `BookView`
- [x] task: data — bar 聚合（时间、笔数、成交量），产出 `...-INTERNAL` bar
- [x] task: data — `FeatureGraph` v0：EMA、VWAP、盘口失衡、microprice、实现波动率，全部定点实现
- [x] task: strategy — `Strategy` concept 与 `Context` 中与数据、时间、定时器相关的方法（§9.4）
- [x] task: strategy — `StaticStrategySet<S...>`、`DynamicStrategySet`、`StrategyVTable`、`JARVIS_REGISTER_STRATEGY`（§7.3）
- [x] task: python — `PyStrategyHost`：按批获取 GIL、异常转为 `StrategyError`、回调计时与超限（§7.4、§7.6）；`on_idle` 移到 M4（backtest 没有空闲期）
- [x] task: python — `jarvis.Strategy` 基类、`jarvis.Node`、`jarvis.main()` 与命令行参数（§4.5）
- [x] task: python — `on_batch` 的列式只读 `nb::ndarray` 视图与 debug 代际检查
- [x] task: backtest — `ReplaySource`（多源合并，按 `(ts, source_id, seq)` 排序）与 `BacktestWiring`
- [x] task: python — 数据转换器：data.binance.vision 的 aggTrades、bookTicker、klines、markPrice 转为解码事件日志
- [x] task: python — 数据转换器：nautilus Parquet 目录与解码事件日志双向转换（pyarrow，§16.5）
- [x] task: tools — `jarvis replay`（`--until`、`--dump-state`）与回放偏差检测 `ReplayDivergence`
- [ ] task: examples — `examples/py/trade_logger.py` 与 `examples/cpp/trade_logger.cpp`：订阅 trade 与 quote，只记录不下单
- [ ] task: harness — golden 回放一致性用例：trade、quote、book、bar、feature
- [ ] task: harness — 零分配门覆盖 `step` 全路径
- [ ] task: harness — 热基准 `step/trade_to_strategy`、`book/apply_l2_delta`、`py/callback_on_quote`，并用实测值更新 architecture §7.8
- [ ] task: harness — Python 确定性测试：同一日志回放两次，命令流与策略输出逐字节一致；确定性守卫拦截墙钟与随机数调用
- [ ] task: 验收 — `python examples/py/trade_logger.py --env backtest` 在一天的 BTCUSDT-PERP 数据上跑通，rel 与 det-o0 指纹一致

## M3 执行、模拟撮合、风控、组合与成本，规约 a/c/d（回测完整可用）

- [ ] task: specs — `OrderLifecycle.tla`（含 jarvis 新增的 `Submitted → Expired`）与 `specs/map/order_lifecycle_actions.hpp`（§8.1、§18）
- [ ] task: execution — `OrderCore` 与订单状态转移表，转移表由规约核对，含 `apply` 阶段的 `previous_status` 规则
- [ ] task: execution — OMS：Netting、成交按 `(symbol, orderId, tradeId)` 去重、in-flight 集合、`open_exposure()`、按策略的归因账本
- [ ] task: execution — `ExecutionEngine`：命令路由、事件推进状态机、`OrderDenied` 回送策略
- [ ] task: execution — `ExecAlgorithm` concept、`AlgoState` 竞技场、`AlgoContext`（子单经 Gate B、生成前查询令牌）与直通模式
- [ ] task: strategy — `Context` 的下单与查询方法、订单意图工厂（§9.4）
- [ ] task: cost — `FeeModel`（档位、BNB 抵扣、资金费）、`SlippageModel`、`LatencyModel`（§11.1）
- [ ] task: backtest — `SimulatedExchange`：实现 `VenueClient` concept、双时间线、延迟事件入队（§12.1、§12.2）
- [ ] task: backtest — 成交模型 `TopOfBookCross` 与 `QueuePosition`（以 `TradeTick` 消耗前方排队量，§12.3）
- [ ] task: backtest — `GTX` 会吃单时拒单、`IOC`/`FOK` 余量过期、STP 三种模式、资金费结算
- [ ] task: backtest — `SimulatedExchange` 的快照查询，使对账代码能在回测中运行
- [ ] task: portfolio — `Portfolio`：定点仓位、余额、`MarginModel`、按 mark 计的未实现盈亏、资金费调整（§11.2）
- [ ] task: risk — `RiskRule` concept、Gate A 与 Gate B 的规则数组、§10.1 的规则目录与拒单原因码
- [ ] task: risk — `TradingState`、允许命令矩阵、KillSwitch（§10.2、§10.3）
- [ ] task: risk — 内核内令牌桶，由定时器事件推进（§10.4）
- [ ] task: risk — 事后监控：日内亏损、回撤、保证金率（§10.5）
- [ ] task: specs — `TradingState.tla`（含令牌桶）与映射头
- [ ] task: specs — `Matching.tla` 与映射头
- [ ] task: tools — `tools/tla/behaviours.py`、`trace_driver`、`jarvis trace-export`（§18.2）
- [ ] task: python — 回测 `RunReport`：成交、手续费、盈亏，并标注所用数据与成交模型
- [ ] task: examples — `examples/py/mm_quote.py` 与 `examples/cpp/pegged_mm.cpp`，只供测试与 soak
- [ ] task: harness — `OrderLifecycle` 正向与反向 trace validation 进入 CI
- [ ] task: harness — `TradingState`、`Matching` 的不变量检查与生成行为进入 CI
- [ ] task: harness — 性质测试：成交守恒、仓位等于成交流之和、`open_exposure()` 等于逐单求和
- [ ] task: harness — 热基准 `oms/apply_order_event`、`risk/gate_a`、`risk/gate_b`、`sim/match_top_of_book`、`sim/match_queue_position`、`step/quote_to_command`
- [ ] task: bench — 准备自托管基准 runner（D21），就绪后把核心路径阈值收紧到 3%
- [ ] task: 验收 — Python 与 C++ 示例策略在 BTCUSDT-PERP 数据上完成回测，跨 preset 指纹一致；规约 a、c、d 在 CI 中通过

## M4 网络、Binance USDⓈ-M 适配器、Codec 与录制，规约 e（sandbox 可用）

- [ ] task: build — 钉定 standalone Asio、picohttpparser、simdjson 的提交，OpenSSL 3 使用系统库，全部只链接到 `jarvis_shell`
- [ ] task: network — `Transport` concept 与 Asio + OpenSSL 实现，每个 IO 线程一个 `io_context`，每连接预分配接收缓冲（§13.2）
- [ ] task: network — RFC 6455 客户端帧层：握手、仅出站掩码、分片重组、ping/pong/close
- [ ] task: network — HTTP/1.1 keep-alive 客户端（picohttpparser 解析响应与 chunked）
- [ ] task: network — `Signer` 接口：HMAC-SHA256 与 Ed25519（`EVP_DigestSign`）
- [ ] task: network — 连接管理：24 小时计划重连、指数退避、`Health*` 事件
- [ ] task: adapter — `Codec` concept 与 `JsonCodec`（simdjson on-demand，数值直接解析为定点，§13.3）
- [ ] task: adapter — 行情流解码：aggTrade → `TradeTick`、bookTicker → `QuoteTick`、markPrice → mark、index、funding，kline → `Bar`，forceOrder → `LiquidationOrder`（§14.2）
- [ ] task: specs — `DepthSync.tla` 与映射头
- [ ] task: adapter — depth 同步状态机与快照请求的专用令牌桶（§14.3）
- [ ] task: adapter — `exchangeInfo` 加载 instrument，由 filters 生成 Gate B 规则（§14.6）
- [ ] task: adapter — 启动检查：持仓模式、杠杆与保证金模式、key 权限与 IP 白名单、服务器时间偏移
- [ ] task: adapter — 用户数据流：listenKey 获取与每 30 分钟续期、`listenKeyExpired` 处理、事件解码、`TRADE_LITE` 与 `ORDER_TRADE_UPDATE` 合并（§8.3、§14.5）
- [ ] task: adapter — WS API：`session.logon`（Ed25519）、`order.place`、`order.modify`、`order.cancel`、`order.status`，错误码映射（§8.2、§14.4）
- [ ] task: adapter — REST 兜底下单与快照接口
- [ ] task: adapter — 权重与订单数令牌桶的 `RateLimitFeedback` 回灌，HTTP 429 退避与 418 处理（§14.7）
- [ ] task: adapter — 核对行情流在 `/public` 与 `/market` 路由间的归属及用户数据流连接地址，写入适配器配置（开放问题）
- [ ] task: live — md-io、ud-io、timer、admin、order-sender 线程，入站、出站与回执 SPSC 环（§7.1）
- [ ] task: live — 原始帧录制与解码日志录制，`jarvis redecode`（§13.4）
- [ ] task: live — `SandboxWiring`：`RingSource`、`SimulatedExchange`（真实定时器，触发写入日志）、`MonotonicClock`
- [ ] task: tools — 深度与成交流录制器可独立运行，用于积累 USDⓈ-M L2 数据（§12.4）
- [ ] task: python — `on_idle(ctx)` 钩子：空闲期按 `idle_hook_ms` 持 GIL 运行，默认 `gc.collect(0)`；`on_start` 后 `gc.freeze()`（§7.4）
- [ ] task: harness — Codec、WebSocket 帧层、HTTP 解析的 fuzz 目标：PR 每个 60 秒，nightly 10 分钟，语料入库
- [ ] task: harness — `tsan` preset 覆盖环与 IO 线程
- [ ] task: harness — testnet 契约测试（nightly）：每类流与每个 WS API 方法的往返
- [ ] task: harness — 环境等价测试：sandbox 录制后以 backtest 回放同一策略文件，命令流逐字节相同（§4.6）
- [ ] task: harness — `DepthSync` 的不变量与生成行为进入 CI
- [ ] task: harness — 热基准 `codec/json_aggTrade`、`codec/json_bookTicker`、`codec/json_depth`、`ring/spsc_roundtrip`
- [ ] task: 验收 — `python examples/py/mm_quote.py --env sandbox` 连续运行 24 小时；录制日志回放逐字节一致；环境等价测试通过

## M5 实盘、对账、运维、内置执行算法与纯 C++ 节点，规约 b（发布 v1.0）

- [ ] task: specs — `Reconciliation.tla`（交易所建模为会重排、重复、延迟消息的进程）与映射头（§18.1）
- [ ] task: execution — 对账协议：先订阅后快照、逐单比对、`userTrades` 合成漏成交、遗留与外部订单处理、置位与 `ReconciliationDiff`（§15.2）
- [ ] task: execution — 断线重连对账与每 60 秒轻量对账（§15.3）
- [ ] task: live — `LiveWiring`：`RingSource`、`SenderSink`、`MonotonicClock`；排空优先级 `admin > 回执 · ud-io > md-io > timer`（§5.5）
- [ ] task: live — core 线程绑核与 busy-poll，IO 线程的 NUMA 亲和
- [ ] task: node — Node 生命周期的 `Syncing`、`Degraded`、`Stopping`、`Faulted` 实盘路径
- [ ] task: risk — `countdownCancelAll`：进入 `Synced` 时武装，每 30 秒续期，关停确认后解除（§10.3）
- [ ] task: persist — WAL 三种模式（`none`、`async`、`barrier`）、`EngineState` 快照与日志截断、崩溃恢复（§16.2、§16.3）
- [ ] task: ops — 遥测：`LogRecord` 格式化为 JSON lines，按 §19.2 暴露 Prometheus 指标
- [ ] task: ops — readiness 与 liveness，admin Unix socket 与全部命令（§19.3）
- [ ] task: ops — `SIGTERM` 优雅关停流程（§19.4）
- [ ] task: ops — 密钥引用解析、key 权限与 IP 白名单检查（§19.1）
- [ ] task: execution — 内置执行算法 `PeggedQuote`：改单与撤单重下的选择、令牌预算感知（§11.4）
- [ ] task: execution — 内置执行算法 `PassiveThenAggressive`
- [ ] task: strategy — `jarvis::node_main<S>`：不链接 Python 的纯 C++ 节点
- [ ] task: python — `node.add_native_strategy()`：Python 启动的混合节点承载注册的 C++ 策略
- [ ] task: specs — 决定是否增加第六个规约 `NodeLifecycle`；若采纳则实现并加入 `MAP.toml`（开放问题）
- [ ] task: harness — `Reconciliation` 正向与反向 trace validation 进入 CI
- [ ] task: harness — 混沌测试：由规约 b 的行为生成断线、重复、乱序、丢消息场景，在 sandbox 与 testnet 执行
- [ ] task: harness — 延迟基准：tick 到命令、命令到 socket 的 p50 与 p99 归档，并据实测设定延迟目标
- [ ] task: harness — nightly：1 小时 sandbox soak，以及对最近一次 soak 日志的反向验证
- [ ] task: docs — 运维手册：部署、配置、密钥、告警与故障处理
- [ ] task: 验收 — Python 与 C++ 示例在 testnet 连续运行 72 小时，穿越强制断线与 listenKey 过期；录制日志回放逐字节一致；五个规约全部在 CI 中通过；小资金生产灰度完成后发布 v1.0

## M6 v1.x：Spot、SBE、更多执行算法与扩展

- [ ] task: adapter — Binance Spot 适配器：`CurrencyPair`、现金账户、`executionReport` 用户流、Spot WS API
- [ ] task: adapter — `SbeCodec`：`sbe-tool` 生成解码器、`specs/sbe/binance/` 中钉版本的 schema、弃用信号处理（§13.3）
- [ ] task: adapter — Spot SBE 行情流（`stream-sbe.binance.com:9443`）与 REST、WS API 的 SBE 响应
- [ ] task: tools — `just sbe-regen` 与 CI `sbe-check`
- [ ] task: execution — `TWAP`（确定性切片 × `PassiveThenAggressive`）、`POV`（`TradeFlowMeter`）、`Iceberg`
- [ ] task: portfolio — `PortfolioConstruction` concept 与参考实现 `TargetPositionRebalancer`（§11.3）
- [ ] task: cost — 基于订单簿深度的 `ImpactModel`
- [ ] task: execution — hedge 模式 OMS（`positionSide = LONG | SHORT`）
- [ ] task: execution — 条件单（STOP、TAKE_PROFIT、TRAILING），同步扩展规约 a
- [ ] task: adapter — COIN-M 合约（inverse）
- [ ] task: backtest — `Probabilistic` 成交模型与强平模拟
- [ ] task: python — 控制面通道：`TargetPosition` 与 `ParamUpdate` 事件，从研究节点驱动执行节点
- [ ] task: execution — 评估 Binance `priceMatch` 作为 `PeggedQuote` 的可选模式
- [ ] task: persist — 评估用 SBE XML 定义日志记录 schema（D17）
- [ ] task: harness — `codec/sbe_trade` 等 SBE 门禁基准、SBE fuzz 语料、规约 d 覆盖新增执行算法
- [ ] task: harness — SBE 与 JSON 对同一段 Spot 数据解码得到相同事件的一致性测试
- [ ] task: 验收 — Python 中频示例策略在 Spot testnet 实盘运行；新增执行算法通过规约 d；SBE 与 JSON 一致性测试通过

## M7 规模化

- [ ] task: live — 按 instrument 分片：每个分片一个 Node，账户级限额的静态预算与控制面再平衡（§19.5）
- [ ] task: specs — 分片预算规约：任一时刻各分片预算之和不超过账户限额
- [ ] task: network — `UringTransport`（`ASIO_HAS_IO_URING` 与 `ASIO_DISABLE_EPOLL`）实验与对比基准（D13）
- [ ] task: live — core 线程内联发送实验（不经 order-sender）
- [ ] task: live — 大页内存与竞技场预取
- [ ] task: python — Python free-threading 解释器下的混合节点实验
- [ ] task: harness — 分片部署的 soak 与跨分片预算不变量检查
- [ ] task: 验收 — 三个分片共用一个账户运行 72 小时，预算不变量未被违反；io_uring 有实测收益后再把 D13 转为 Accepted

## 模块覆盖

| 模块 | 里程碑 |
| --- | --- |
| build、ci、justfile、tools、tests、testkit、bench | M0 起，每个里程碑追加 |
| core | M1 |
| model | M1 |
| log、persist | M1（日志格式）、M5（WAL 模式与恢复）、M6（schema 评估） |
| config、node | M1（配置与 backtest 生命周期）、M5（实盘生命周期） |
| data | M2 |
| strategy | M2（数据接口与分发）、M3（下单接口）、M5（纯 C++ 节点） |
| engine | M2 |
| backtest | M2（回放）、M3（撮合）、M6（概率成交与强平） |
| execution | M3（状态机、OMS、执行引擎）、M5（对账、内置算法）、M6（更多算法、hedge、条件单） |
| cost | M3、M6 |
| portfolio | M3、M6 |
| risk | M3、M5 |
| specs | M0（骨架）、M3（a、c、d）、M4（e）、M5（b）、M7（分片预算） |
| network | M4、M7 |
| adapter | M4（USDⓈ-M）、M6（Spot、SBE、COIN-M） |
| live | M4（线程与 sandbox）、M5（实盘）、M7（分片） |
| ops | M5 |
| python | M1（绑定与守卫）、M2（宿主与转换器）、M3（报告）、M5（混合节点）、M6（控制面）、M7（free-threading） |
| examples | M2、M3 |
| docs | M0、M5 |
