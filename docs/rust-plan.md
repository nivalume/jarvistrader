# Rust 迁移实施计划

本计划按 [rust-migration.md](rust-migration.md) 第 7 节的移植顺序拆成 R0 到 R6 七个里程碑。约定与 [plan.md](plan.md) 相同：

1. 每个任务以"crate —"开头，crate 名与 `rust/Cargo.toml` 的 workspace 成员一致；`harness` 表示测试、基准或形式化验证任务，`验收` 是该里程碑的完成标准。
2. **C++ 树是设计参考，不是 oracle。** C++ 版本没有生产使用，Rust 树不以逐字节复现它为目标，而是按 architecture.md 的契约（nautilus 领域模型、ADR 0001 的确定性约束、TLA+ 规约）给出自己的实现。正确性由三样东西决定：测试（单元、性质、golden、模糊）、形式化验证（规约与 trace validation）、确定性门（release 与 det-o0 逐字节一致）。可以借用 C++ 的测试向量和 golden 数据作为输入，不借用它的字节格式。
3. Rust 树在 `rust/`，与 C++ 树并行，不混进同一个二进制。R6 之前 Python 包仍加载 C++ 扩展；R4 为 Rust 的事件日志写 Python 读取器。
4. 门禁：`just rust-check`（格式、clippy 作为错误、dev / release / det-o0 三个 profile 的测试、内核 crate 的 `no_std` 目标检查、`rust-fp` 确定性门），CI 的 `rust` job 跑同样的内容并进入 `gate`。

状态：R0 完成，R1 的 core 与 model 层完成（事件日志、指纹、corpus 在内）；本文随每个里程碑更新。

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

## R1 core、model 与事件日志（领域模型契约、确定性编码、指纹门）

> 2026-10-08 core 层第一遍完成。`core` 覆盖 `jarvis/core` 的全部 13 个头文件：`Status`（`Result<T, Status>`，无 `Ok` 变体）、192 位中间值的 `mul_div`、时间与 RFC 3339、`EventKey`、`FixedVec`、`FixedString<N>`、带代际句柄的槽位表、Philox 计数随机数、`PriorityQueue`（含 `min_where`、`retain`）、`ReplayClock` 与 `TimerQueue`、CRC-32C（slicing-by-8 查表）、SHA-256、快照编码（`State` trait、`state_fields!`/`state_enum!`）。`tests/cpp/test_core.cpp` 的 unit、property、zero-alloc 三组全部移植，另加快照编码布局与往返测试，共 28 个用例，三个 profile 通过；Philox、SHA-256、CRC-32C、RFC 3339 的已知答案向量与 C++ 测试相同。
>
> 2026-10-08 core 层第二遍：按 Rust 的方式重写第一遍翻译自 C++ 的部分，不要求与第一遍字节兼容，测试、clippy、三个 profile 与确定性门全部通过。改动：`Arena` 改名 `SlotMap`（它是带代际句柄的槽位表，不是分配器），槽位存 `Option<T>`，去掉了 `T: Default` 才能构造的限制和"删除后值留在原地"的 C++ 语义，`remove` 返回值，提供 `iter`/`iter_mut`/`values`/`Index`，快照恢复拒绝重复或指向占用槽位的空闲表项；`FixedVec` 新增 `retain`、`from_iter_exact`、`IntoIterator`，快照读取不再需要私有钩子；`FixedString` 在构造时校验 UTF-8；RFC 3339 的解析与格式化从 `const fn` 改为普通函数，用 `?` 与游标类型取代手写的位置参数，`UnixNanos` 实现 `Display` 与 `FromStr`；`mul_div` 的 192 位中间值改为 `(hi: u64, lo: u128)`，`_up` 用余数判定而不是重算乘积，`isqrt` 改为牛顿法；`TimerQueue` 的存活判定收敛到一个 `live` 方法，`pop_due` 不再有嵌套的早退；`PriorityQueue::retain` 复用 `FixedVec::retain`；`state.rs` 去掉了为引用私有类型而留的占位函数。
>
> 2026-10-08 model 层完成。`model` 是独立设计，不参照 C++ 的 `jarvis/model`：
> - 定点数值：`Price { raw: i64, precision }`、`Quantity { raw: u64, precision }`、`Money { raw: i64, currency }`，raw 按 1e9 刻度，相等与比较只看 raw；构造函数拒绝低于 precision 的数位，因此"precision 只影响显示"是不变量而不是约定。文本解析接受 `[+-]digits[.digits][e[+-]digits]`，精度按写出的小数位推断，超过 9 位报 `PrecisionLoss`；指定精度时 round half to even；`Money` 向零截断到币种精度。`notional_raw` 经 192 位中间值精确计算并向零截断。
> - 币种表由 `rust/tools/gen_conformance.py` 从 `tests/conformance/nautilus_cd417b80.json` 生成（91 种），枚举 28 个保留 nautilus 的整数值、字符串与别名，解析不区分大小写，拒绝 `NO_*` 旧 token。
> - 标识符各有规则（`InstrumentId` 按最后一个 `.` 拆分，`TraderId` 按最后一个 `-`，`AccountId` 按第一个 `-`），全部定长内联。`ClientOrderId` 格式 `{tag}-{epoch:6}-{seq:8}`，Base32 定宽，按下单顺序排序，可解码，最长 36 字符。
> - 行情、instrument、账户、17 种订单事件、4 种仓位事件，每个类型有校验构造函数；封闭的 `Event` 枚举 17 种输入，`EventKind` 带线上 tag 与分类。
> - 线格式 `Wire` trait：小端定宽逐字段，解码一律经过校验构造函数，因此"能解码的就是合法值"，且 `encode(decode(b)) == b`（规范编码）。事件日志：日志头（魔数、格式与 schema 版本、配置 hash、seed、标签，带 CRC）加记录（长度、kind、EventKey、body、CRC-32C）；截断与损坏分别报 `Truncated` 与 `ChecksumMismatch`，之前的记录仍可读。指纹是记录规范编码的 SHA-256 加计数，与日志头文本、分段无关。
> - corpus（`crates/testkit/corpus`）：`(seed, 记录号, 用途, 序号)` 键控的 Philox 生成器产出全部事件种类与全部订单事件变体，键严格递增；任一记录可单独重算。
>
> 验收结果：nautilus 一致性测试 4 个、model 测试 21 个通过（dev、release、det-o0）；性质测试覆盖文本往返、notional 对 i128 算术、ClientOrderId 往返、corpus 上的规范编码（两个种子各 400 条，含全部前缀截断与尾部多余字节）；日志测试覆盖最后一条记录的 4 个截断点、body 翻转一位、日志头损坏；seed 7 的 20 万条 corpus 在 release 与 det-o0 下逐字节相同（24.6 MB），指纹入 golden，`just rust-fp` 与 CI 复核；20 万条记录解码加指纹 0.29 秒。内核 crate 继续在 `x86_64-unknown-none` 上编译。
>
> 与 C++ 树的有意差异（不再是缺陷）：日志线格式与 C++ 的不兼容；`Status` 没有 `Ok` 变体；`RecordFlag`、`BookType` 的 Rust 变体名是驼峰（字符串不变）；CRC-32C 只有查表实现。

- [x] task: core — `Status`、`FixedVec`（超容返回 `CapacityExceeded`）、`SlotMap` 与带代际的 32 位句柄
- [x] task: core — counter-based RNG（splitmix64、Philox），键为 `(seed, identity, hop, index)`
- [x] task: core — `UnixNanos`、`DurationNanos` 与 RFC 3339 格式化、解析
- [x] task: core — `EventKey` 与确定性优先队列
- [x] task: core — `Clock` trait、`ReplayClock` 与定时器队列
- [x] task: core — CRC-32C、SHA-256
- [x] task: core — 快照编码：`State` trait、读写器、容器编码、`state_fields!` / `state_enum!`
- [x] task: harness — core 层的 unit、property、zero-alloc 测试；零分配门在 Rust 树上运行
- [x] task: core — 第二遍：去除翻译痕迹（见上方记录）
- [x] task: model — `Price`、`Quantity`、`Money`、`Currency`：raw 按 1e9 刻度、文本解析与格式化、`i128` / 192 位乘法与向零截断（architecture.md §6.1）
- [x] task: model — 全部标识符及其字符串约束（§6.2）；`InstrumentId` intern 为槽位的侧表留给 data 层（R2，它属于路由）
- [x] task: model — 全部枚举，保留 nautilus 的整数值与字符串（§6.6）；快照与线格式编码由宏给出
- [x] task: model — 行情数据类型（§6.4）、Instrument（§6.5）、订单事件、仓位事件、账户事件（§6.7）
- [x] task: model — `ClientOrderId` 生成器 `{node_tag}-{epoch}-{seq}`（Base32）与解码（§8.4）
- [x] task: model — 封闭的 `Event` 枚举与事件分类（§5.1）
- [x] task: model — 事件日志：日志头、记录布局、CRC-32C（§5.6、§16.1）；按段滚动与 `fdatasync` 属于 node 层（R4）
- [x] task: model — 指纹（`Fingerprinter`）与确定性 corpus
- [x] task: cli — `jarvis-rs corpus`、`fingerprint`、`dump`、`roundtrip`
- [x] task: tools — `gen_conformance.py`：从 nautilus 一致性 JSON 生成币种表与测试向量；CI 检查生成物是最新的
- [x] task: harness — nautilus 一致性测试（枚举值、字符串、别名、常量、币种、字段表）；定点算术与编码的性质测试；日志损坏测试
- [x] task: harness — 指纹门：release 与 det-o0 对 seed 7 的 20 万条 corpus 逐字节一致，指纹入 golden（`just rust-fp`，CI）
- [ ] task: model — `OrderBookDepth`（可变档数的簿快照类型）；v1.0 的 Binance 路径用 `OrderBookDeltas`，暂缓
- [ ] task: model — 字段描述符（schema）：文本输出目前用 `Debug`，稳定的字段级文本与 Python 读取器一起在 R4 做
- [ ] task: harness — 模糊测试目标 `decimal`、`wire`、`log`（cargo-fuzz，corpus 作为种子）；需要在 CI 中引入 nightly 或 `cargo-fuzz` 的 stable 路径
- [ ] task: core — CRC-32C 硬件路径：放在 shell 的一个 `#[allow(unsafe_code)]` 模块中，以 `is_x86_feature_detected!` 选择；内核保持查表实现
- [ ] task: 验收 — 以上三项完成；Python（R4 的读取器）能读 Rust 写出的日志且指纹一致

## R2 data、cost、portfolio（行情类 golden 通过）

- [ ] task: data — `Router` 与 `SubscriptionMatrix`，类型化 `Subscription { slot, kind, cadence }`（§7.2）
- [ ] task: data — `Cadence`：`Every`、`Conflated`、`SampledNs`、`OnBatch`，以及 `BatchEnd`（§7.5）
- [ ] task: data — 订单簿 L1 与 L2：按 tick 索引的稠密价位表示，只读 `BookView`；`write_sparse` / `read_sparse` 快照编码
- [ ] task: data — bar 聚合（时间、笔数、成交量）
- [ ] task: data — `FeatureGraph`：EMA、VWAP、盘口失衡、microprice、实现波动率，全部定点
- [ ] task: cost — `FeeModel`、`SlippageModel`、`ImpactModel`、`LatencyModel`（§11.1）
- [ ] task: portfolio — `Portfolio`、`MarginModel`、归因账本（§11.2）
- [ ] task: harness — 对应层的 C++ 测试移植；基准 `book/apply_l2_delta`、`step/trade_with_feature` 的 Rust 版本（criterion）与 C++ 数字并列报告
- [ ] task: 验收 — 路由、订单簿、bar 聚合、特征各有性质测试与 Rust 树自己的 golden（输入是 corpus 或由 `python -m jarvis.data` 转换的一天 Binance 数据，输出指纹入 `rust/tests/golden/`）；release 与 det-o0 指纹一致

## R3 execution、risk、strategy、engine、backtest（订单类 golden 与 trace validation 通过）

- [ ] task: execution — `OrderCore`、订单状态机转移表（`match` 穷尽）、OMS、`ExecutionEngine`（§8）
- [ ] task: execution — 执行算法 `PeggedQuote`、`PassiveThenAggressive`、`AlgoState` 竞技场、令牌预算（§11.4）
- [ ] task: execution — 对账：`VenueSnapshot`、`ReconciliationDiff`、`ReconcileOutcome`（§15）
- [ ] task: risk — 规则目录、Gate A 与 Gate B、预留敞口、`TradingState`、`TokenBucket`、事后监控（§9、§10）
- [ ] task: strategy — `Strategy` trait、`Context`、`StaticStrategySet`（泛型）与 `DynamicStrategySet`（`dyn Strategy`，对应 C++ 的函数指针表）
- [ ] task: engine — `Engine<S>`、`EngineState`、`EventSource` 与 `CommandSink` trait、`step(S, e) → (S′, out[])`
- [ ] task: backtest — `ReplaySource`（多源合并）、`VenueLoop`、`SimulatedExchange`、双时间线、成交模型枚举（§12）
- [ ] task: specs — `specs/map/*.hpp` 改为 Rust 模块；`tests/trace/` 的行为文件由 Rust 驱动运行
- [ ] task: harness — 零分配门覆盖每个 `step`；对应层测试移植；`step/trade_to_strategy` 基准
- [ ] task: 验收 — 订单生命周期、撮合、风控的 Rust golden 通过；快照保存与恢复逐字节往返；七个规约的正向 trace validation 通过，`tests/trace/` 的行为文件由 Rust 驱动复用

## R4 node、sys、Python 绑定（Python 示例零改动运行）

- [ ] task: jarvis-sys — 崩溃安全的文件写入、阻塞 socket、共享库加载、停止信号、进程与环境变量、单调时钟（§3 sys）
- [ ] task: jarvis-node — `NodeConfig`：toml + serde、`deny_unknown_fields`、`--env` 与 `--set` 覆盖、规范化与 hash 与 C++ 一致（§4.2）
- [ ] task: jarvis-node — Node 生命周期状态机、`BacktestNode`、构建信息、`jarvis-rs replay` / `config` / `report`
- [ ] task: jarvis-node — 快照文件与恢复（§16.3）
- [ ] task: python — `jarvis-py` crate（PyO3 + maturin，abi3）：绑定全部模型类型、`PyStrategyHost`、`on_batch` 的只读 ndarray 视图、`jarvis.log` 读写；`python/jarvis/` 包按构建选项加载 C++ 或 Rust 扩展
- [ ] task: harness — `python/tests/` 对 Rust 扩展全部通过；`tests/golden/node_config` 通过
- [ ] task: python — `jarvis.log` 的纯 Python 读取器读 Rust 事件日志（线格式见 `model::log`），指纹与 `jarvis-rs fingerprint` 一致
- [ ] task: 验收 — `examples/py/` 零改动运行；回放 Rust 节点自己的日志逐字节复现输出

## R5 network、adapter、live（脚本化服务端与混沌测试通过）

- [ ] task: jarvis-network — tokio + rustls + fastwebsockets 的 `WsClient`、hyper 的 `HttpsClient`、`Signer`；读空闲期限与 ping（§13）
- [ ] task: jarvis-binance — `JsonCodec`（sonic-rs 或 simd-json）、流映射、订单簿同步、WS API、用户数据流、REST、启动检查、限速（§14）
- [ ] task: jarvis-live — 环：热路径三条用 rtrb 传 `Event` / `QueuedCommand`，persist 用有界队列，冷路径 std mpsc；`Waker`；绑核（rust-migration.md §6.4）
- [ ] task: jarvis-live — `MarketFeed`、`VenueIo`、`OrderTracker`、`Persister`、telemetry、admin、健康检查、`SandboxNode`、`LiveNode`（§7.1、§19）
- [ ] task: jarvis-live — 原始帧录制与 `jarvis-capture redecode`（§13.4）
- [ ] task: harness — loom 覆盖环（若自写）与 `Waker` 的交换协议；miri 覆盖全部 `unsafe`；脚本化 HTTPS / WSS 服务端移植；混沌测试与延迟基准移植
- [ ] task: 验收 — 混沌测试两个种子通过；`redecode --check` 对自己录制的原始帧产出与运行日志一致的解码日志；sandbox 录制日志回放逐字节复现输出

## R6 切换

- [ ] task: python — wheel 默认加载 Rust 扩展；C++ 扩展作为 `JARVIS_IMPL=cpp` 的回退保留一个版本
- [ ] task: build — 删除 C++ 树、CMake、CPM、clang 配置；`just check` 的各层改为 Rust 配方；CI 作业图改为 lint → rust → {determinism, bench-compare, formal} → gate
- [ ] task: docs — architecture.md 的"执行手段"改为 Rust 机制；D12、D18 改写；cpp-subset.md 退役，替换为 Rust 树的 lint 清单
- [ ] task: 验收 — testnet 72 小时验收在 Rust 节点上重复一次
