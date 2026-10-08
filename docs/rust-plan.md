# Rust 迁移实施计划

本计划按 [rust-migration.md](rust-migration.md) 第 7 节的移植顺序拆成 R0 到 R6 七个里程碑。约定与 [plan.md](plan.md) 相同：

1. 每个任务以"crate —"开头，crate 名与 `rust/Cargo.toml` 的 workspace 成员一致；`harness` 表示测试、基准或门禁任务，`验收` 是该里程碑的完成标准。
2. 每个里程碑的验收都是对 C++ 树的逐字节比较：C++ 树是 oracle，`tests/golden/` 与 corpus 指纹是跨实现的等价测试。Rust 树不定义自己的"正确"。
3. 事件日志线格式、`jarvis fingerprint` 算法、配置规范化 hash 三样在整个迁移期间不变；Python 包的公开 API 不变。
4. Rust 树在 `rust/`，与 C++ 树并行，不混进同一个二进制。R6 之前 Python 包仍加载 C++ 扩展。
5. 门禁：`just rust-check`（格式、clippy 作为错误、dev / release / det-o0 三个 profile 的测试、内核 crate 的 `no_std` 目标检查），CI 的 `rust` job 跑同样的内容并进入 `gate`。

状态：R0 完成，R1 的 core 层完成；本文随每个里程碑更新。

## R0 workspace 与门禁（在写领域代码之前，让约束先变成编译错误）

> 2026-10-08 完成。验收结果：向内核 crate 注入 `f64` 乘法，clippy 报 `floating-point arithmetic detected`；注入 `std::collections::HashMap` 与 `alloc::collections::BTreeMap`，报 `use of a disallowed type`；注入 `unsafe` 块，`forbid(unsafe_code)` 拒绝编译；在零分配作用域内分配一个 `Vec`，门报 1 次分配；release profile 下 `200u8 + 100u8` panic（`overflow-checks` 在所有 profile 打开）。`cargo check` 以 `x86_64-unknown-none` 目标编译全部内核 crate 通过，证明内核不依赖 std。与方案的一处偏差：溢出用 `overflow-checks` 而不是 clippy `arithmetic_side_effects`，后者会把每个循环计数器都标出来；两者在方案第 3 节里是并列选项。一处未落实：clippy 的 `disallowed-methods` 不能指向原生切片的固有方法，`sort_unstable*` 的禁令暂由评审维持。

- [x] task: build — `rust/Cargo.toml` workspace：内核十个 crate、shell 五个 crate、testkit、cli；`[workspace.lints]` 统一 clippy 级别；`overflow-checks` 在 dev、release 全开；`det-o0` profile 继承 release 并关优化（对应 C++ 的 `det-o0` preset）
- [x] task: build — 内核 crate 的契约写在每个 `lib.rs` 顶部：`#![no_std]`、`#![forbid(unsafe_code)]`、`#![deny(clippy::float_arithmetic)]`；分层靠 Cargo 依赖图（`data` 与 `cost` 同级不互相依赖，与 architecture.md 第 3 节的表一致）
- [x] task: build — `rust/crates/kernel/clippy.toml`：内核禁 `HashMap`、`HashSet`、`BTreeMap`；`rust/clippy.toml`：全 workspace 禁 `std::time::{SystemTime, Instant}`（时间只能来自 `jarvis-sys`）
- [x] task: build — `rust-toolchain.toml`（stable）、`rustfmt.toml`、`deny.toml`（内核零第三方依赖；shell 依赖按许可证白名单）
- [x] task: testkit — `jarvis-testkit`：`Gen`（与 C++ `testkit::Gen` 同一 splitmix64 流）、`for_all` 与 `JARVIS_PROP_SEED` / `JARVIS_PROP_ITERS` / `JARVIS_PROP_CASE`、`CountingAlloc` 全局分配器与 `AllocationScope`（零分配门）
- [x] task: cli — `jarvis-rs` 二进制骨架：`build-info`、`sha256`、`crc32c`；其余子命令随各层落地
- [x] task: ci — `rust` job：fmt、clippy `-D warnings`、三个 profile 的测试、`no_std` 目标检查；进入 `gate`
- [x] task: justfile — `rust-check`、`rust-lint`、`rust-test`、`rust-nostd`
- [x] task: 验收 — 分别注入浮点、`HashMap`、`unsafe`、作用域内分配、溢出，五道门各自报错；内核 crate 在无 std 目标上编译

## R1 core、model 与事件日志（corpus 指纹逐字节一致）

对应 C++ 的 M1。验收是 `jarvis-rs corpus --seed 7 --events 200000` 与 C++ `jarvis corpus` 的输出逐字节相同，以及 `tests/golden/model_strings`、`corpus_seed42` 通过。

> 2026-10-08 core 层完成。`jarvis-core` 移植了 `jarvis/core` 的全部 13 个头文件：`Status`（`Result<T, Status>`，不再有 `Ok` 变体，名字与编号与 C++ 一致）、`int_math`（`mul_div_u64` / `_up` / `_i64` 经 192 位中间值、`isqrt`）、`time`（`UnixNanos`、`DurationNanos`、civil 日期、RFC 3339 格式化与解析）、`EventKey`、`FixedVec`、`FixedString<N>`、`Arena` 与代际 `Handle<Tag>`、`rng`（`mix64`、Philox4x32-10、`CounterRng`）、`PriorityQueue`（含 `min_where`、`retain`）、`ReplayClock` 与 `TimerQueue`、CRC-32C（slicing-by-8 查表，与 C++ 的硬件指令结果相同）、SHA-256、`state`（`StateWriter` / `StateReader` / `State` trait、`state_fields!` 与 `state_enum!` 宏，字节布局与 `core/state.hpp` 一致，含 `FixedVec`、`Arena`、`PriorityQueue`、`TimerQueue` 的编码）。`tests/cpp/test_core.cpp` 的 unit、property、zero-alloc 三组全部移植为 `rust/crates/kernel/jarvis-core/tests/core.rs`，另加快照编码的字节布局测试与 Arena / TimerQueue 的快照往返，共 28 个用例，dev、release、det-o0 三个 profile 全部通过；Philox、SHA-256、CRC-32C、RFC 3339 的已知答案向量与 C++ 测试相同。

- [x] task: jarvis-core — `Status`、`FixedVec`（超容返回 `CapacityExceeded`）、slab 竞技场与带代际的 32 位句柄
- [x] task: jarvis-core — counter-based RNG（splitmix64、Philox），键为 `(seed, identity, hop, index)`
- [x] task: jarvis-core — `UnixNanos`、`DurationNanos` 与 RFC 3339 格式化、解析
- [x] task: jarvis-core — `EventKey` 与确定性优先队列
- [x] task: jarvis-core — `Clock` trait、`ReplayClock` 与定时器队列
- [x] task: jarvis-core — CRC-32C、SHA-256
- [x] task: jarvis-core — 快照编码：`State` trait、读写器、容器编码、`state_fields!` / `state_enum!`
- [x] task: harness — `test_core.cpp` 的全部用例移植；零分配门在 Rust 树上运行
- [ ] task: jarvis-core — CRC-32C 硬件路径：放在 shell 的一个 `#[allow(unsafe_code)]` 模块中，以 `is_x86_feature_detected!` 选择；内核保持查表实现；两者的一致性由 property 测试覆盖
- [ ] task: jarvis-model — `Price`、`Quantity`、`Money`、`Currency`：raw 按 1e9 刻度、字符串解析与格式化、`i128` 乘法与向零截断（architecture.md §6.1）；对照 `tests/conformance/nautilus_cd417b80.json`
- [ ] task: jarvis-model — 全部标识符及其字符串约束，`InstrumentId` intern 为 `u32` 槽位的侧表（§6.2）
- [ ] task: jarvis-model — 全部枚举，保留 nautilus 的整数值与字符串（§6.6）；`state_enum!` 给出快照编码
- [ ] task: jarvis-model — 行情数据类型（§6.4）、Instrument（§6.5）、订单事件、仓位事件、账户事件（§6.7）
- [ ] task: jarvis-model — `ClientOrderId` 生成器 `{node_tag}-{epoch}-{seq}`（Base32）与解码（§8.4）
- [ ] task: jarvis-model — 封闭的 `Event` 枚举与事件分类（§5.1）；`Output` 枚举
- [ ] task: jarvis-model — 字段描述符（对应 `model/schema.hpp`）：日志编码、文本输出与 Python 绑定共用
- [ ] task: jarvis-model — 事件日志：日志头、记录布局、CRC-32C、按段滚动（§5.6、§16.1），线格式与 C++ 逐字节相同
- [ ] task: cli — `jarvis-rs corpus`、`fingerprint`、`dump`、`roundtrip`
- [ ] task: harness — 定点算术性质测试、nautilus 字符串格式往返 golden、模糊测试目标 `decimal` 与 `wire`（cargo-fuzz，复用 `tests/fuzz/corpus/`）
- [ ] task: harness — 指纹门：release 与 det-o0 对同一语料的事件日志逐字节一致
- [ ] task: 验收 — Rust 与 C++ 对 seed 7 的 20 万条语料输出逐字节相同；`tests/golden/model_strings` 与 `corpus_seed42` 通过；Python（`jarvis.log`）能读 Rust 写出的日志且指纹不变

## R2 data、cost、portfolio（行情类 golden 通过）

- [ ] task: jarvis-data — `Router` 与 `SubscriptionMatrix`，类型化 `Subscription { slot, kind, cadence }`（§7.2）
- [ ] task: jarvis-data — `Cadence`：`Every`、`Conflated`、`SampledNs`、`OnBatch`，以及 `BatchEnd`（§7.5）
- [ ] task: jarvis-data — 订单簿 L1 与 L2：按 tick 索引的稠密价位表示，只读 `BookView`；`write_sparse` / `read_sparse` 快照编码
- [ ] task: jarvis-data — bar 聚合（时间、笔数、成交量）
- [ ] task: jarvis-data — `FeatureGraph`：EMA、VWAP、盘口失衡、microprice、实现波动率，全部定点
- [ ] task: jarvis-cost — `FeeModel`、`SlippageModel`、`ImpactModel`、`LatencyModel`（§11.1）
- [ ] task: jarvis-portfolio — `Portfolio`、`MarginModel`、归因账本（§11.2）
- [ ] task: harness — 对应层的 C++ 测试移植；基准 `book/apply_l2_delta`、`step/trade_with_feature` 的 Rust 版本（criterion）与 C++ 数字并列报告
- [ ] task: 验收 — `replay_quote`、`replay_trade`、`replay_book`、`replay_bar`、`replay_feature` golden 通过（需要 R3 的最小 engine 驱动；若 R3 未到，先以 C++ 日志为输入、比较 Rust 路由与特征的输出记录）

## R3 execution、risk、strategy、engine、backtest（订单类 golden 与 trace validation 通过）

- [ ] task: jarvis-execution — `OrderCore`、订单状态机转移表（`match` 穷尽）、OMS、`ExecutionEngine`（§8）
- [ ] task: jarvis-execution — 执行算法 `PeggedQuote`、`PassiveThenAggressive`、`AlgoState` 竞技场、令牌预算（§11.4）
- [ ] task: jarvis-execution — 对账：`VenueSnapshot`、`ReconciliationDiff`、`ReconcileOutcome`（§15）
- [ ] task: jarvis-risk — 规则目录、Gate A 与 Gate B、预留敞口、`TradingState`、`TokenBucket`、事后监控（§9、§10）
- [ ] task: jarvis-strategy — `Strategy` trait、`Context`、`StaticStrategySet`（泛型）与 `DynamicStrategySet`（`dyn Strategy`，对应 C++ 的函数指针表）
- [ ] task: jarvis-engine — `Engine<S>`、`EngineState`、`EventSource` 与 `CommandSink` trait、`step(S, e) → (S′, out[])`
- [ ] task: jarvis-backtest — `ReplaySource`（多源合并）、`VenueLoop`、`SimulatedExchange`、双时间线、成交模型枚举（§12）
- [ ] task: specs — `specs/map/*.hpp` 改为 Rust 模块；`tests/trace/` 的行为文件由 Rust 驱动运行
- [ ] task: harness — 零分配门覆盖每个 `step`；对应层测试移植；`step/trade_to_strategy` 基准
- [ ] task: 验收 — `replay_orders`、`replay_batch`、`snapshot_orders`、`example_trade_logger`、`example_pegged_mm` golden 通过；`EngineState` 快照与 C++ 逐字节相同；六个规约的正向 trace validation 通过

## R4 node、sys、Python 绑定（Python 示例零改动运行）

- [ ] task: jarvis-sys — 崩溃安全的文件写入、阻塞 socket、共享库加载、停止信号、进程与环境变量、单调时钟（§3 sys）
- [ ] task: jarvis-node — `NodeConfig`：toml + serde、`deny_unknown_fields`、`--env` 与 `--set` 覆盖、规范化与 hash 与 C++ 一致（§4.2）
- [ ] task: jarvis-node — Node 生命周期状态机、`BacktestNode`、构建信息、`jarvis-rs replay` / `config` / `report`
- [ ] task: jarvis-node — 快照文件与恢复（§16.3）
- [ ] task: python — `jarvis-py` crate（PyO3 + maturin，abi3）：绑定全部模型类型、`PyStrategyHost`、`on_batch` 的只读 ndarray 视图、`jarvis.log` 读写；`python/jarvis/` 包按构建选项加载 C++ 或 Rust 扩展
- [ ] task: harness — `python/tests/` 对 Rust 扩展全部通过；`tests/golden/node_config` 通过
- [ ] task: 验收 — `examples/py/` 零改动运行，运行日志与 C++ 树逐字节相同；`python -m jarvis.data` 的转换器产出与 C++ 树可互读

## R5 network、adapter、live（脚本化服务端与混沌测试通过）

- [ ] task: jarvis-network — tokio + rustls + fastwebsockets 的 `WsClient`、hyper 的 `HttpsClient`、`Signer`；读空闲期限与 ping（§13）
- [ ] task: jarvis-binance — `JsonCodec`（sonic-rs 或 simd-json）、流映射、订单簿同步、WS API、用户数据流、REST、启动检查、限速（§14）
- [ ] task: jarvis-live — 环：热路径三条用 rtrb 传 `Event` / `QueuedCommand`，persist 用有界队列，冷路径 std mpsc；`Waker`；绑核（rust-migration.md §6.4）
- [ ] task: jarvis-live — `MarketFeed`、`VenueIo`、`OrderTracker`、`Persister`、telemetry、admin、健康检查、`SandboxNode`、`LiveNode`（§7.1、§19）
- [ ] task: jarvis-live — 原始帧录制与 `jarvis-capture redecode`（§13.4）
- [ ] task: harness — loom 覆盖环（若自写）与 `Waker` 的交换协议；miri 覆盖全部 `unsafe`；脚本化 HTTPS / WSS 服务端移植；混沌测试与延迟基准移植
- [ ] task: 验收 — 混沌测试两个种子通过；`redecode --check` 对 C++ 树录制的原始帧产出逐字节相同的解码日志；sandbox 对同一段实盘行情，Rust 与 C++ 节点的运行日志在扣除 `ts_init` 之后逐字节相同

## R6 切换

- [ ] task: python — wheel 默认加载 Rust 扩展；C++ 扩展作为 `JARVIS_IMPL=cpp` 的回退保留一个版本
- [ ] task: build — 删除 C++ 树、CMake、CPM、clang 配置；`just check` 的各层改为 Rust 配方；CI 作业图改为 lint → rust → {determinism, bench-compare, formal} → gate
- [ ] task: docs — architecture.md 的"执行手段"改为 Rust 机制；D12、D18 改写；cpp-subset.md 退役，替换为 Rust 树的 lint 清单
- [ ] task: 验收 — testnet 72 小时验收在 Rust 节点上重复一次
