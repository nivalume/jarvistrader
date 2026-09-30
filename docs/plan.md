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
- [x] task: examples — `examples/py/trade_logger.py` 与 `examples/cpp/trade_logger.cpp`：订阅 trade 与 quote，只记录不下单；两者对同一数据写出逐字节相同的运行日志（`python/tests/test_examples.py`）
- [x] task: harness — golden 回放一致性用例：trade、quote、book、bar、feature（另有 batch 与 C++ 示例；`tests/golden/replay_*`、`tests/golden/example_trade_logger`）
- [x] task: harness — 零分配门覆盖 `step` 全路径
- [x] task: harness — 热基准 `step/trade_to_strategy`、`book/apply_l2_delta`、`py/callback_on_quote`，并用实测值更新 architecture §7.8（`py/*` 为只报告基准：A/B 门禁作业不构建 Python）
- [x] task: harness — Python 确定性测试：同一日志回放两次，命令流与策略输出逐字节一致；确定性守卫拦截墙钟与随机数调用
- [x] task: 验收 — `python examples/py/trade_logger.py --env backtest` 在一天的 BTCUSDT-PERP 数据上跑通，rel 与 det-o0 指纹一致

M2 验收记录（`tools/m2_acceptance.sh`）：2024-03-30 的 BTCUSDT-PERP（data.binance.vision 的 aggTrades 57 万条、bookTicker 740 万条；该站的 USDⓈ-M bookTicker 归档只发布到 2024 年春季）经 `jarvis.data` 转换后，由 Python 与 C++ 两个 trade logger 示例分别在 Release 与 `-O0` 构建下回测。四份运行日志逐字节相同（12,658,983 条记录，1257 万条输入、8.9 万条输出），Python 运行回放无偏差。

## M3 执行、模拟撮合、风控、组合与成本，规约 a/c/d（回测完整可用）

- [x] task: specs — `OrderLifecycle.tla`（含 jarvis 新增的 `Submitted → Expired`）与 `specs/map/order_lifecycle_actions.hpp`（§8.1、§18）
- [x] task: execution — `OrderCore` 与订单状态转移表，转移表由规约核对，含 `apply` 阶段的 `previous_status` 规则
- [x] task: execution — OMS：按 `ClientOrderId` 开放寻址索引、成交按 trade id 去重、已关闭订单按关闭顺序淘汰、按 instrument 与策略统计未完成数量（Netting）
- [x] task: execution — `open_exposure()`（venue 持仓 + 未完成订单，OMS 按 instrument 维护运行总量）与按策略的归因账本；父单剩余量随执行算法加入
- [x] task: execution — `ExecutionEngine`：命令作为输出、venue 事件推进状态机、`OrderDenied` 与内核产生的订单事件回送策略的 `on_order_event`
- [x] task: execution — `ExecAlgorithm` concept、`AlgoState` 竞技场、`AlgoContext`（子单经 Gate B、生成前查询限速额度）与直通模式
- [x] task: strategy — `Context` 的下单方法（`submit`、`modify`、`cancel`、`cancel_all`）、订单与 instrument 查询、意图工厂，C++ 与 Python 同名（§9.4）
- [x] task: strategy — `Context` 的 `position`、`balance`、`exposure`、`trading_state` 查询，C++ 与 Python 同名
- [x] task: cost — `FeeModel`（档位、BNB 抵扣、资金费）、`SlippageModel`、`LatencyModel`（§11.1）
- [x] task: backtest — `SimulatedExchange`：实现 `VenueClient` concept、双时间线、延迟事件入队（§12.1、§12.2）
- [x] task: backtest — 成交模型 `TopOfBookCross` 与 `QueuePosition`（以 `TradeTick` 消耗前方排队量，§12.3）
- [x] task: backtest — `GTX` 会吃单时拒单、`IOC`/`FOK` 余量过期、STP 三种模式、资金费结算
- [x] task: backtest — `SimulatedExchange` 的快照查询（未完成订单、仓位、余额），使对账代码能在回测中运行
- [x] task: portfolio — `Portfolio`：定点仓位、余额、`MarginModel`、按 mark 计的未实现盈亏、资金费调整（§11.2）
- [x] task: risk — `RiskRule` concept、Gate A 与 Gate B 的规则数组、§10.1 的规则目录与拒单原因码
- [x] task: risk — `TradingState`、允许命令矩阵、KillSwitch（§10.2、§10.3）
- [x] task: risk — 内核内限速（按时钟对齐的固定窗口，由输入的 ts 推进，§10.4）
- [x] task: risk — 事后监控：日内亏损、回撤、保证金率（§10.5）
- [x] task: specs — `TradingState.tla`（含限速窗口）与映射头
- [x] task: specs — `Matching.tla`（排队位置成交模型）与映射头
- [x] task: tools — `tools/tla/behaviours.py`、`trace_driver`、`jarvis trace-export`、`tools/tla/check_trace.py`（§18.2）
- [x] task: python — 回测 `RunReport`：成交、手续费、盈亏，并标注所用数据与成交模型（C++ `jarvis report` 与 Python `result.report()` 共用一份实现）
- [x] task: examples — `examples/py/mm_quote.py` 与 `examples/cpp/pegged_mm.cpp`，只供测试与 soak（二者为孪生实现，同一数据写出逐字节相同的日志）
- [x] task: harness — `OrderLifecycle` 正向与反向 trace validation 进入 CI（反向用 golden 用例 `replay_orders` 的日志）
- [x] task: harness — `TradingState`、`Matching` 的不变量检查与生成行为进入 CI
- [x] task: harness — 性质测试：成交守恒、仓位等于成交流之和、`open_exposure()` 等于逐单求和
- [x] task: harness — 热基准 `oms/apply_order_event`、`risk/gate_a`、`risk/gate_b`、`step/quote_to_command`
- [x] task: harness — 热基准 `sim/match_top_of_book`、`sim/match_queue_position`
- [ ] task: bench — 准备自托管基准 runner（D21），就绪后把核心路径阈值收紧到 3%
- [x] task: 验收 — Python 与 C++ 示例策略在 BTCUSDT-PERP 数据上完成回测，跨 preset 指纹一致；规约 a、c、d 在 CI 中通过

M3 验收记录（`tools/m3_acceptance.sh`）：2024-03-30 的 BTCUSDT-PERP（aggTrades 57 万条、bookTicker 740 万条，instrument 定义按 BTCUSDT 公布的过滤器写入目录）由 Python 做市示例 `mm_quote.py` 与其 C++ 孪生 `pegged_mm` 分别在 Release 与 `-O0` 构建下对模拟 venue 回测（queue_position 成交模型、带抖动的延迟、VIP0 费率）。四份运行日志逐字节相同（13,762,710 条记录，1372 万条输入、4 万条输出，其中 6.9 万条 venue 回报），Python 运行回放无偏差。该运行的 20,697 个订单投影到 `OrderLifecycle` 后共 109,689 步（其中 51 步是实现拒绝的迟到事件），TLC 判定为规约的合法行为。`RunReport`：29,255 笔成交全部为 maker，成交名义 1391 万 USDT，手续费 2781.78 USDT，已实现 -1011.45 USDT；示例策略只在触价挂单、不做选择，亏损符合预期。这次运行暴露了模拟撮合的一个缺陷：`QueuePosition` 按比例扣减前方量时没有取整到数量步长，之后的成交量落在网格外，运行因 `PrecisionLoss` 停止；修正后前方量向下取整到订单的步长，并加了回归测试。真实数据中还有少量不在 tick 上的成交价（如 tick 为 0.1 时的 70344.83），转换器因此以两位小数记录成交价，内核与撮合器按原值处理。规约 a、c、d 已接入 CI 的 formal job（TLC、正向回放、golden trace 的反向检查），三项在本地均通过；本分支的推送不触发 CI，CI 上的结果尚未验证。

## M4 网络、Binance USDⓈ-M 适配器、Codec 与录制，规约 e（sandbox 可用）

- [x] task: build — 钉定 standalone Asio、picohttpparser、simdjson 的提交，OpenSSL 3 使用系统库，全部作为私有依赖链接到 `jarvis_network`（`JARVIS_BUILD_LIVE`；CI 在 Linux 安装 `libssl-dev`，macOS 用 Homebrew 的 `openssl@3`）
- [ ] task: network — `Transport` concept 与 Asio + OpenSSL 实现，每个 IO 线程一个 `io_context`，每连接预分配接收缓冲（§13.2）：每个 IO 线程一个 `IoContext`、每连接预分配接收缓冲已随 `WsClient` 实现；`Transport` concept（SBE、测试替身共用的抽象）随 M6 的 SBE 传输
- [x] task: network — RFC 6455 客户端帧层：握手、仅出站掩码、分片重组、ping/pong/close（`ws_frame`、`WsClient`，含 TLS 回环测试与 `on_close` 内重连测试）
- [x] task: network — HTTP/1.1 keep-alive 客户端（picohttpparser 解析响应与 chunked；`HttpsClient` 阻塞式，超时与一次重试）
- [x] task: network — `Signer` 接口：HMAC-SHA256 与 Ed25519（`EVP_DigestSign`），密钥引用 `env:`、`file:`
- [ ] task: network — 连接管理：24 小时计划重连、指数退避、`Health*` 事件：WS API 与用户流会话已实现先建后拆的 24 小时轮换与退避重连，行情连接退避重连；`Health*` 事件推动 `Degraded → Syncing`，而 `Syncing` 就是对账协议，所以随 M5 的断线重连对账实现
- [x] task: adapter — `Codec` concept 与 `JsonCodec`（simdjson on-demand，数值直接解析为定点，§13.3）：`jarvis/adapter/codec.hpp`、`jarvis/adapter/binance/json_codec.hpp`；只用有序字段查找（fuzz 发现无序查找回绕的未定义行为）
- [x] task: adapter — 行情流解码：aggTrade → `TradeTick`、bookTicker → `QuoteTick`、markPrice → mark、index、funding，kline → `Bar`，forceOrder → `LiquidationOrder`（§14.2）；depth 帧解码为 `DepthDiff`，同步后产出 `OrderBookDeltas` 随 M4-C
- [x] task: specs — `DepthSync.tla` 与映射头（`specs/tla/DepthSync.tla`、`specs/map/depth_sync_actions.hpp`；交易所簿抽象建模，含丢事件、断线与滞后快照）
- [x] task: adapter — depth 同步状态机与快照请求的专用令牌桶（§14.3）：`DepthSync`、`DepthBooks`（共享 `risk::RateLimiter` 预算、轮流取用），REST 与 WS API 快照解码，每侧档位上限。2026-09-27 生产环境实测（`jarvis-capture depth-check`，快照经 WS API）：BTCUSDT 5 分钟 19 次、ETHUSDT 3 分钟 11 次独立重同步，与主簿在同一更新号的前 100 档全部相同，0 缺口、0 解码错误；加上每侧 2000 档上限后 BTCUSDT 再跑 3 分钟 11 次，同样全部相同（不设上限时 5 分钟内每侧增长到约 3300 档）
- [x] task: adapter — `exchangeInfo` 加载 instrument，由 filters 生成 Gate B 规则（§14.6）：filters 落到 `InstrumentCommon` 字段，Gate B 规则直接读取；与 Python 目录映射对同一 testnet 夹具逐字段一致
- [x] task: adapter — 启动检查：持仓模式、杠杆与保证金模式、key 权限与 IP 白名单、服务器时间偏移：`run_startup_checks`（另含 exchangeInfo 与多资产模式；只读，一次列出全部问题），对脚本化 HTTPS 服务端测试
- [x] task: adapter — 用户数据流：listenKey 获取与每 30 分钟续期、`listenKeyExpired` 处理、事件解码、`TRADE_LITE` 与 `ORDER_TRADE_UPDATE` 合并（§8.3、§14.5）：事件解码、`OrderTracker`、Lite 成交与手续费补记（内核 `Oms::take_pending_commission`、`Portfolio::on_commission`）、余额表合并；`UserStreamSession`（订阅确认后 live、断线重连、轮换重叠期按全文去重）与 `ListenKeyKeeper`（续期、失败或过期时重新申请）。把 `listenKeyExpired` 接到 keeper 随 M4-E 的运行时
- [x] task: adapter — WS API：`session.logon`（Ed25519）、`order.place`、`order.modify`、`order.cancel`、`order.status`，错误码映射（§8.2、§14.4）：请求构造与签名、应答解码（`requests.hpp`），`WsApiSession`（登录、按 id 配对、确认/拒绝/未知三种结局、超时、断线重连、先建后拆的 24 小时轮换；`order.status` 等走通用 `request`），对 mock venue 测试；2026-09-27 对生产 `ws-fapi` 用 `jarvis-capture ws-api-probe` 验证应答格式
- [x] task: adapter — REST 兜底下单与快照接口：`RestClient`（签名、时间偏移、listenKey、持仓模式与 positionRisk、depth 快照、下单改单撤单；4xx 带码为拒绝，5xx 与超时为结果未知），对脚本化 HTTPS 服务端测试
- [ ] task: adapter — 权重与订单数令牌桶的 `RateLimitFeedback` 回灌，HTTP 429 退避与 418 处理（§14.7）：内核事件 `RateLimitFeedback` 与 `RateLimiter::feedback`、响应头与 `rateLimits` 解析、418 封禁期间快速失败已完成；sandbox 对模拟交易所下单，没有交易所的限速反馈，接到内核随 M5 的 `LiveWiring`
- [x] task: adapter — 核对行情流在 `/public` 与 `/market` 路由间的归属及用户数据流连接地址，写入适配器配置（开放问题）：路由已于 2026-09-27 实测并写入 `jarvis/adapter/binance/streams.hpp`（`/stream` 只剩 `/public` 的流）；用户数据流为 `/private/stream` 加 `SUBSCRIBE` listenKey，取自 Binance 官方 SDK 17.5.0，没有账户无法在线验证，由 testnet 契约测试补上
- [ ] task: live — md-io、ud-io、timer、admin、order-sender 线程，入站、出站与回执 SPSC 环（§7.1）：`SpscRing`、`SpscByteRing`、md-io 线程（`MarketFeed`）与 core 实时循环（`Driver::run_realtime`）已完成；不设单独的 timer 线程（内核定时器由 core 循环按时钟触发，网络定时器在各 IO 线程上）；信号经 `ShutdownSignals` 变成停止请求；WS API 与用户流合在 venue-io 线程上（`jarvis/live/venue_io.hpp`，出站命令环、账户入站环、REST 线程负责 listenKey 与对账快照），接入实盘节点随 M5-C3
- [x] task: live — 原始帧录制与解码日志录制，`jarvis redecode`（§13.4）：sandbox 运行目录含 `raw-frames.jraw`（WS API 快照请求与应答也写入，新增 `Sent` 记录）；`jarvis-capture redecode --check` 验证重解码的行情与运行日志的行情输入逐字节相同（生产 40 秒会话 2704 条全部相同）
- [x] task: live — `SandboxWiring`：`RingSource`、`SimulatedExchange`（真实定时器，触发写入日志）、`MonotonicClock`：`run_sandbox`（`LiveSource`、`VenueLoop` 的 `live_feed`、`MonotonicClock`），C++ 入口 `live_node_main`，Python 入口在带 live shell 的构建中（`just install-live`）
- [x] task: tools — 深度与成交流录制器可独立运行，用于积累 USDⓈ-M L2 数据（§12.4）：`jarvis-capture record` 写原始帧文件（断线自动重连），`decode` 离线解码核对；由原始帧重建解码日志随 M4-E 的 `jarvis redecode`。2026-09-27 实测：BTCUSDT、ETHUSDT 在 `/public` 与 `/market` 两个连接上采集 10 分钟，207,681 条消息离线解码为 196,539 个事件与 11,737 个 depth 差量，0 错误，depth 的 `pu` 链无缺口
- [x] task: python — `on_idle(ctx)` 钩子：空闲期按 `idle_hook_ms` 持 GIL 运行，默认 `gc.collect(0)`；`on_start` 后 `gc.freeze()`（§7.4）：`python.idle_hook_ms`（默认 100）；`on_idle()` 不接收 ctx（在 step 之外运行，无法回放），运行结束时 `gc.unfreeze()`
- [x] task: harness — Codec、WebSocket 帧层、HTTP 解析的 fuzz 目标：PR 每个 60 秒，nightly 10 分钟，语料入库（`fuzz_ws`、`fuzz_http`、`fuzz_json_codec`，种子取自实盘采集）
- [x] task: harness — `tsan` preset 覆盖环与 IO 线程：nightly 的 tsan job 跑全部 ctest，包括环的跨线程压力测试、WS 会话与 sandbox 端到端测试
- [ ] task: harness — testnet 契约测试（nightly）：每类流与每个 WS API 方法的往返
- [x] task: harness — 环境等价测试：sandbox 录制后以 backtest 回放同一策略文件，命令流逐字节相同（§4.6）：`test_sandbox`（脚本化交易所）与内核层实时对 backtest 测试；生产行情上 C++ 与 Python 示例的回放均无偏差
- [x] task: harness — `DepthSync` 的不变量与生成行为进入 CI：TLC 不变量、正向 trace validation（ctest 与 formal job、nightly 由 MAP.toml 决定规约清单），以及以内核 `OrderBook` 为观察者的随机交易所性质测试
- [x] task: harness — 热基准 `codec/json_aggTrade`、`codec/json_bookTicker`、`codec/json_depth`、`ring/spsc_roundtrip`：codec 三项已进 `bench_codec` 门禁组（本机 Release 中位数约 290 ns、330 ns、3.2 µs，depth 为 46 档的 1.5 KB 消息）；`ring/spsc_roundtrip` 与 `ring/byte_record` 在 `bench_live` 门禁组（本机约 690 ns 与 9 ns）
- [ ] task: 验收 — `python examples/py/mm_quote.py --env sandbox` 连续运行 24 小时；录制日志回放逐字节一致；环境等价测试通过：环境等价测试通过；2 小时生产行情 soak 通过（见下方记录）；24 小时连续运行尚未进行

M4 验收记录（2026-09-27）：Python 示例 `mm_quote.py` 以 `--env sandbox` 在生产行情（`fstream.binance.com` 的 `aggTrade`、`bookTicker`、`depth@100ms`，快照经 `ws-fapi.binance.com`）上对模拟交易所连续运行 2 小时（带 live shell 的 release wheel）。运行正常结束（`STOPPED`）：4,564,971 条输入，其中行情 2,747,364 条、交易所回报 6,639 条，输出 3,024 条；行情线程 2,747,404 条消息 0 解码错误、0 次环满等待、1 次快照、1 次订单簿同步，全程没有断线。`mm_quote.py --replay` 复算全部输出无偏差；`jarvis-capture redecode --check` 由 781 MB 原始帧重建的行情与运行日志中的 2,747,364 条行情输入逐字节相同（另有 37 条在运行停止时尚未步进）。instrument 定义使用 testnet 的 exchangeInfo 夹具并按 BTCUSDT 生产环境的数量过滤器（步长与最小数量 0.001、最小名义价值 100）修改，因为本环境访问生产 REST（`fapi.binance.com`）返回 451。第一次 soak 在 3.5 分钟时以 `CapacityExceeded` 停止：回放无偏差，说明内核没有问题，原因是模拟交易所自己的 L2 簿容量（窗口 4096 档、窗口外每侧 1024 档）小于内核的簿，而实时 depth 流每侧保留最多 2000 档、许多远离盘口；修正后模拟交易所的簿容量取自内核配置，并加了回归测试。

## M5 实盘、对账、运维、内置执行算法与纯 C++ 节点，规约 b（发布 v1.0）

- [x] task: specs — `Reconciliation.tla`（交易所建模为会重排、重复、延迟消息的进程）与映射头（§18.1）：规约与 TLC 检查（三个协议变体都被不变量抓到）；成交只通过计数改变状态（与 OMS 一致）；映射头 `specs/map/reconciliation_actions.hpp` 与正向 trace validation（`MAP.toml` 中 `forward = true`）
- [ ] task: execution — 对账协议：先订阅后快照、逐单比对、`userTrades` 合成漏成交、遗留与外部订单处理、置位与 `ReconciliationDiff`（§15.2）：内核一侧已完成（`jarvis/execution/reconciliation.hpp`：会话阶段、暂存、按快照置位、合成事件、差异与结果、`on_reconciled`、过期状态事件；单元与性质测试）；适配器用 REST 组装 `VenueSnapshot`（一致读）已完成（`jarvis/adapter/binance/snapshot.hpp`），接入 venue-io 线程随 `LiveWiring`
- [x] task: execution — 断线重连对账与每 60 秒轻量对账（§15.3）：断线重连已完成（内核会话阶段与 driver 同步闸门：`Running → Degraded → Syncing → Running`）；每 60 秒轻量对账已完成（M5-E：venue-io 读取 `openOrders` 与 `positionRisk`，记录为 `check = true` 的 `VenueSnapshot`；内核在 `Synced` 时比较、排除 T_c 前 5 秒内的变化、连续两次出现才确认，输出 `ReconciliationDiff` 并降为 `Reducing`；新差异种类 `UNTRACKED_ORDER`）
- [ ] task: live — `LiveWiring`：`RingSource`、`SenderSink`、`MonotonicClock`；排空优先级 `admin > 回执 · ud-io > md-io > timer`（§5.5）：实盘节点已接线（`jarvis/live/live_node.hpp`：凭证、启动检查、epoch、`MarketFeed` 与 `VenueIo`、账户环先于行情环、`CommandRouter`、`await_sync`；C++ `live_node_main` 与 Python `Node.run`；脚本化交易所端到端测试与回放一致）；WS API 不可用时的 REST 下单兜底已完成（M5-F）；admin 通道随 ops
- [ ] task: live — core 线程绑核与 busy-poll，IO 线程的 NUMA 亲和
- [x] task: node — Node 生命周期的 `Syncing`、`Degraded`、`Stopping`、`Faulted` 实盘路径：`Syncing` 与 `Degraded` 由同步闸门驱动（`engine/sync_gate.hpp`，`DriverOptions::await_sync`）；`Stopping` 的撤单与等待已完成（M5-D2）；行情与下单通道的连接状态接入同步闸门已完成（M5-D3：md-io 记录 `ConnectionStatus(MarketData)`，内核 `ConnectionHealth`，down 时 `Running → Degraded`，实盘等下单通道 up 才进入 `Running`）；行情新鲜度与 `Faulted` 已完成（M5-L：内核周期定时器按 `[node] market_data_stale_ms` 判断行情陈旧，`Running → Degraded`，行情恢复后回到 `Running`；driver 在任何错误时尽力 KillSwitch、记录 `Fault` 转移与批次结束，返回原错误，`RunSummary::faulted`）
- [x] task: risk — `countdownCancelAll`：进入 `Synced` 时武装，每 30 秒续期，关停确认后解除（§10.3）：武装与续期已完成（M5-D1：内核输出 `CountdownCancelAll`，进入 `Running` 时与内核定时器每 30 秒覆盖有挂单的 instrument，新单所在 instrument 未被覆盖时同一步补上；只在 `env = "live"` 打开；venue-io 的 REST 线程发出；引擎单元测试与脚本化交易所端到端测试）；关停确认后解除已完成（M5-D2）
- [x] task: persist — WAL 三种模式（`none`、`async`、`barrier`）、`EngineState` 快照与日志截断、崩溃恢复（§16.2、§16.3）：三种模式已完成（M5-I1：sandbox 与 live 的 persist 线程 `Persister`，按 `sync_every_ms` 或每批 `fdatasync` 并公布 durable 位置；`barrier` 下 venue-io 等命令的记录落盘后才发出；日志尾部截断的容忍读取）；`EngineState` 快照已完成（M5-I2：内核全部状态的定宽显式编码与每个策略自己的状态（C++ `state(ar)`，Python `on_save`/`on_load`），每 `snapshot_every` 条输入在批次边界取快照，sandbox 与 live 由 persist 线程在日志落盘后写出；回放逐字节核对每个快照，`--from-snapshot` 从快照回放；引擎层与对账层的性质测试、全部 golden 场景逐快照回放、确定性门比较 Release 与 `-O0` 的快照；live 运行的 epoch 记录为 `RunStart` 输入）；截断与崩溃恢复已完成（M5-I3：`persistence.resume` 从上次运行的最近完整快照恢复并按回放核对其后的日志，容忍崩溃留下的半条记录与缺失的输出，新运行写起始快照、`seq` 接着编号、以 `RunStart{epoch, prior_seq}` 重置会话后重新对账；恢复的未结订单交给 venue-io 的订单跟踪器；`persistence.truncate` 在完整快照后删除旧段与旧快照，回放自动从覆盖日志开头的快照开始；定时器查询不再改变状态）
- [x] task: ops — 遥测：`LogRecord` 格式化为 JSON lines，按 §19.2 暴露 Prometheus 指标：已完成（M5-J：内核每步的 `LogRecord`（TradingState 变化、策略停用、KillSwitch）；`TelemetryRecorder` 把输入、输出与内核记录放进遥测环，遥测线程写 `telemetry.jsonl`（按 `client_order_id` 串起订单）并提供 `/metrics`、`/ready`、`/live`；§19.2 表中各项指标，含 step、tick 到命令、命令到连接三个整数纳秒直方图；单元测试与 sandbox、live 端到端测试）
- [x] task: ops — readiness 与 liveness，admin Unix socket 与全部命令（§19.3）：admin socket（0600）、记录的 `AdminCommand` 输入（`halt`、`reduce`、`resume`、`cancel_all`、`shutdown`）、`status`（ready 与 alive）、`jarvis admin` 已完成（M5-G，sandbox 端到端测试含回放一致）；`set_param`（记录的 `ParamUpdate` 输入与 `on_params_changed`）与 `snapshot`（下一个批次边界取快照，回放同样）已完成（M5-K）
- [ ] task: ops — `SIGTERM` 优雅关停流程（§19.4）：已完成（M5-D2，`[node] shutdown` 与 `shutdown_timeout_ms`：记录的 `Shutdown` 输入、`Halted` 与 KillSwitch、`Stopping` 中等待确认或超时、`on_stop` 后不再回调、`Stopped` 时解除 `countdownCancelAll`、`RunSummary::left_open`；引擎、driver 与脚本化交易所端到端测试）；关停时的最终快照未做（恢复从最近的快照重算其后的日志，不依赖它）
- [x] task: ops — 密钥引用解析、key 权限与 IP 白名单检查（§19.1）：`env:` 与 `file:` 引用解析（`jarvis/node/credentials.hpp`）、key 权限与 IP 白名单检查（启动检查）已完成；凭证文件权限检查已完成（M5-D4：保存 secret 的文件须为 0600 或 0400）
- [x] task: execution — 内置执行算法 `PeggedQuote`：改单与撤单重下的选择、令牌预算感知（§11.4）：已实现（M5-H：同方向最优价或中间价加偏移、post-only 且不越过对手价、改价跟随、限速余额保留、被拒或被撤后重挂）；队列位置已完成（M5-M：`AlgoTop` 带最优档数量，按比例扣减的前方量估计，队首订单向市场方向多等一个价位；未确认时不改单；交易所拒绝改单时撤单重下）
- [x] task: execution — 内置执行算法 `PassiveThenAggressive`：已实现（M5-H：post-only 挂单、超时或偏离后撤单并以 IOC 吃单、吃单份额上限；引擎测试与 Python 模拟交易所回测含回放一致）
- [x] task: strategy — `jarvis::node_main<S>`：不链接 Python 的纯 C++ 节点：已完成（`jarvis/node/node_main.hpp`，示例 `trade_logger` 与 `pegged_mm` 只链接 `jarvis::shell`（live 版另加 `jarvis::live`），不依赖 libpython）
- [x] task: python — `node.add_native_strategy()`：Python 启动的混合节点承载注册的 C++ 策略：已完成（按名创建注册的策略；M5-N：`jarvis.load_native(path)` 加载 `jarvis_add_strategy_plugin` 构建的插件，入口报告编译器与共享类型的布局指纹，不一致即拒绝；C++ 与 Python 端到端测试）
- [x] task: specs — 决定是否增加第六个规约 `NodeLifecycle`；若采纳则实现并加入 `MAP.toml`（开放问题）：已采纳（M5-O：`specs/tla/NodeLifecycle.tla`，转移表、同步闸门、关停与撤单等待、`Faulted`、策略回调与未完成订单；TLC 检查不变量与 `Stopping` 的终结；`MAP.toml` 条目与正向 trace validation，trace driver 运行真实的实时 driver，ctest `trace.NodeLifecycle`；反向验证留待实盘日志）
- [ ] task: harness — `Reconciliation` 正向与反向 trace validation 进入 CI：正向已进入 ctest（`trace.Reconciliation`）、CI formal job 与 nightly；反向待实盘日志
- [ ] task: harness — 混沌测试：由规约 b 的行为生成断线、重复、乱序、丢消息场景，在 sandbox 与 testnet 执行
- [x] task: harness — 只报告基准 `snapshot/save_state`、`snapshot/load_state`（`bench_report`，默认容量的内核，一个永续合约、每侧 200 档的订单簿、200 张订单中 100 张未结）：本机 Release 状态 229 KB，保存约 1.08 ms，恢复约 0.94 ms；快照在批次边界的 core 线程上编码，所以每 `snapshot_every` 条输入有一次约 1 ms 的停顿
- [ ] task: harness — 延迟基准：tick 到命令、命令到 socket 的 p50 与 p99 归档，并据实测设定延迟目标
- [ ] task: harness — nightly：1 小时 sandbox soak，以及对最近一次 soak 日志的反向验证
- [ ] task: docs — 运维手册：部署、配置、密钥、告警与故障处理
- [ ] task: 验收 — Python 与 C++ 示例在 testnet 连续运行 72 小时，穿越强制断线与 listenKey 过期；录制日志回放逐字节一致；全部规约在 CI 中通过；小资金生产灰度完成后发布 v1.0

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
