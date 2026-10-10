# Rust 迁移实施计划

本计划按 [rust-migration.md](rust-migration.md) 第 7 节的移植顺序拆成 R0 到 R6 七个里程碑。约定与 [plan.md](plan.md) 相同：

1. 每个任务以"crate —"开头，crate 名与 `rust/Cargo.toml` 的 workspace 成员一致；`harness` 表示测试、基准或形式化验证任务，`验收` 是该里程碑的完成标准。
2. **C++ 树是设计参考，不是 oracle。** C++ 版本没有生产使用，Rust 树不以逐字节复现它为目标，而是按 architecture.md 的契约（nautilus 领域模型、ADR 0001 的确定性约束、TLA+ 规约）给出自己的实现。正确性由三样东西决定：测试（单元、性质、golden、模糊）、形式化验证（规约与 trace validation）、确定性门（release 与 det-o0 逐字节一致）。可以借用 C++ 的测试向量和 golden 数据作为输入，不借用它的字节格式。
3. Rust 树在 `rust/`，与 C++ 树并行，不混进同一个二进制。R6 之前 Python 包仍加载 C++ 扩展；R4 为 Rust 的事件日志写 Python 读取器。
4. 门禁：`just rust-check`（格式、clippy 作为错误、dev / release / det-o0 三个 profile 的测试、内核 crate 的 `no_std` 目标检查、`rust-fp` 确定性门），CI 的 `rust` job 跑同样的内容并进入 `gate`。

状态：R0、R1（除待办项）、R2 完成，R3 第一阶段完成（执行算法、对账、事后监控顺延到 R3 第二阶段）；本文随每个里程碑更新。

## R0 workspace 与门禁（在写领域代码之前，让约束先变成编译错误）

> 2026-10-08 完成。验收结果：向内核 crate 注入 `f64` 乘法，clippy 报 `floating-point arithmetic detected`；注入 `std::collections::HashMap` 与 `alloc::collections::BTreeMap`，报 `use of a disallowed type`；注入 `unsafe` 块，`forbid(unsafe_code)` 拒绝编译；在零分配作用域内分配一个 `Vec`，门报 1 次分配；release profile 下 `200u8 + 100u8` panic（`overflow-checks` 在所有 profile 打开）。`cargo check` 以 `x86_64-unknown-none` 目标编译全部内核 crate 通过，证明内核不依赖 std。与方案的一处偏差：溢出用 `overflow-checks` 而不是 clippy `arithmetic_side_effects`，后者会把每个循环计数器都标出来；两者在方案第 3 节里是并列选项。一处未落实：clippy 的 `disallowed-methods` 不能指向原生切片的固有方法，`sort_unstable*` 的禁令暂由评审维持。

- [x] task: build — `rust/Cargo.toml` workspace：内核十个 crate、runtime 五个 crate（architecture.md 里的 shell 层）、testkit、cli；`[workspace.lints]` 统一 clippy 级别；`overflow-checks` 在 dev、release 全开；`det-o0` profile 继承 release 并关优化（对应 C++ 的 `det-o0` preset）
- [x] task: build — 内核 crate 的契约写在每个 `lib.rs` 顶部：`#![no_std]`、`#![forbid(unsafe_code)]`、`#![deny(clippy::float_arithmetic)]`；分层靠 Cargo 依赖图（`data` 与 `cost` 同级不互相依赖，与 architecture.md 第 3 节的表一致）
- [x] task: build — `rust/crates/kernel/clippy.toml`：内核禁 `HashMap`、`HashSet`、`BTreeMap`；`rust/clippy.toml`：全 workspace 禁 `std::time::{SystemTime, Instant}`（时间只能来自 `sys`）
- [x] task: build — `rust-toolchain.toml`（stable）、`rustfmt.toml`、`deny.toml`（内核零第三方依赖；runtime 依赖按许可证白名单）
- [x] task: testkit — `testkit`：`Gen`（与 C++ `testkit::Gen` 同一 splitmix64 流）、`for_all` 与 `JARVIS_PROP_SEED` / `JARVIS_PROP_ITERS` / `JARVIS_PROP_CASE`、`CountingAlloc` 全局分配器与 `AllocationScope`（零分配门）
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
- [ ] task: core — CRC-32C 硬件路径：放在 runtime 的一个 `#[allow(unsafe_code)]` 模块中，以 `is_x86_feature_detected!` 选择；内核保持查表实现
- [ ] task: 验收 — 以上三项完成；Python（R4 的读取器）能读 Rust 写出的日志且指纹一致

## R2 data、cost、portfolio（行情类 golden 通过）

> 2026-10-08 完成。三个 crate 都是独立设计，验收是测试、性质与 Rust 树自己的 golden：
> - `data`：`InternTable`/`InstrumentTable`/`BarTable` 按首次出现分配槽位；`SubscriptionMatrix` 行 × 12 种 `DataKind`，单元内按订阅顺序，`Cadence::{Every, Conflated, Sampled, OnBatch}` 的判定在 `Subscriber::on_update` / `on_batch_end`，Sampled 的周期对齐 Unix 纪元；`route_of` 把 17 种事件映射到 (row, kind) 或 None；`OrderBook` 按 tick 索引，每侧 64 的倍数个稠密档位带占用位图，窗外档位在有序溢出表，触碰离开窗口中半时重定中心，L1 由 quote 驱动、L2 由 delta 驱动且两者互斥；`BarAggregator` 支持 TICK、VOLUME（整单位，跨阈值拆分）、时间 bar（对齐纪元、在 end 收盘、`next_close` 供引擎定时器）；`FeatureGraph` 的 EMA、VWAP、Imbalance、Microprice、RealizedVol 全部定点，相同声明共享一个 id。每个有状态类型都有快照编码。
> - `cost`：`MakerTakerFees` 内置五个档位，手续费按对账户不利方向取整到币种精度（支付向上、返佣向下），符号与 `OrderFilled::commission` 一致（支付为正）；`funding` 返回账户现金流（收到为正，多头支付正费率）；`BookDepthSlippage` 逐档吃单，平均价按对吃单方不利方向取整；`JitteredLatency` 是 `(seed, identity, hop, attempt)` 的纯函数，用独立派生的 Philox 密钥。
> - `portfolio`：`NettingPosition` 以带符号数量加 10^18 刻度的开仓名义记账，均价精确，全平实现的盈亏正好是出入场名义之差，减仓按比例去除成本；`Portfolio` 维护 venue 仓位、按 (策略, instrument) 的账本、余额、估值价（mark 否则最后成交）、资金费待结算状态，翻转的成交拆成平仓与开仓两部分各自产生事件，`ledger_is_consistent` 断言 venue 仓位恒等于各策略之和；`MarginModel::{Standard, Leveraged}` 向上取整。只记账线性合约。
>
> 验收结果：data 13 个测试（含随机增量对排序参考模型的性质测试、Imbalance/Microprice 的界、seed 7 corpus 前 2 万条派生数据的 golden `data_seed7.fingerprint`），cost 5 个（手续费数值与取整、资金费方向、滑点取整、延迟的纯函数性与范围），portfolio 7 个（精确均价与实现盈亏、奇数数量的比例去除、翻转拆分与余额、资金费结算时机、保证金取整、40 笔随机成交后 venue 等于策略之和且实现盈亏与参考模型一致），三个 profile 全部通过；内核 crate 与 corpus 在 `x86_64-unknown-none` 上编译；corpus 改为按符号固定价格精度（簿与 bar 依赖网格），R1 的两份 corpus golden 相应重新生成。为此在 model 里补了值类型的快照编码（`state_io.rs`，经线格式），枚举得到占位用的 `Default`，`FixedVec` 得到 `insert_at`/`remove_at`。
>
> 未做：`OrderBookDepth` 类型（随 R1 待办）；bar 的 `Completed` 最多装 4 根，一笔超过 4 个 VOLUME 步长的成交会丢 bar 并计数，引擎接入时决定是否改为回调。


- [x] task: data — `Router` 与 `SubscriptionMatrix`，类型化 `Subscription { slot, kind, cadence }`（§7.2）
- [x] task: data — `Cadence`：`Every`、`Conflated`、`SampledNs`、`OnBatch`，以及 `BatchEnd`（§7.5）
- [x] task: data — 订单簿 L1 与 L2：按 tick 索引的稠密价位表示，只读 `BookView`；`write_sparse` / `read_sparse` 快照编码
- [x] task: data — bar 聚合（时间、笔数、成交量）
- [x] task: data — `FeatureGraph`：EMA、VWAP、盘口失衡、microprice、实现波动率，全部定点
- [x] task: cost — `FeeModel`、`SlippageModel`、`ImpactModel`、`LatencyModel`（§11.1）
- [x] task: portfolio — `Portfolio`、`MarginModel`、归因账本（§11.2）
- [x] task: harness — 各层的单元与性质测试、corpus 派生数据 golden
- [ ] task: harness — 基准 `book/apply_l2_delta`、`step/trade_with_feature`（criterion，需要引入第一个第三方 dev 依赖；与 R3 的引擎基准一起做）
- [x] task: 验收 — 路由、订单簿、bar 聚合、特征各有性质测试与 Rust 树自己的 golden（输入是 corpus 或由 `python -m jarvis.data` 转换的一天 Binance 数据，输出指纹入 `rust/tests/golden/`）；release 与 det-o0 指纹一致

## R3 execution、risk、strategy、engine、backtest（订单类 golden 与 trace validation 通过）

> 2026-10-10 第一阶段完成。五个 crate 按 `docs/architecture.md` §8–§12 独立实现，规约是转移关系的唯一来源：
> - `model`：新增 `outputs.rs`（`Output::{FeatureUpdate, StrategyRecord, SubmitOrder, ModifyOrder, CancelOrder, CancelAllOrders, OrderDenied, CountdownCancelAll}`，带线编码）与 `NodeLifecycle` 输入事件（`NodeState`、`LifecycleReason` 枚举）；日志记录分为输入与输出两类（`flags` 位 0），`LogWriter::append_output`、`Fingerprinter::add_output`，`SCHEMA_VERSION` 升为 2，三份 corpus/data golden 相应重新生成。
> - `execution`：`fsm.rs` 的 82 条转移三元组由测试解析 `specs/tla/OrderLifecycle.tla` 的 `BEGIN/END TRANSITIONS` 核对；`OrderState` 实现 Plain/Updated/Fill/Void 与 `SavePrev` 规则；`Oms` 以固定容量持有订单（线性探测 id 表、成交记录池、已关闭订单淘汰环、按 slot 的在途数量），快照只存订单与成交，id 表与在途合计在恢复时重建；`apply_order_event` 给出 `Applied/UnknownOrder/Refused/DuplicateFill/Stale`。
> - `risk`：`TradingStateMachine`（base、同步保持、降级保持）与 `MATRIX`、触发表由测试对照 `TradingState.tla` 的 `TRIGGERS/MATRIX`；`RateLimiter` 固定窗口；`checks.rs` 结构检查原因码；`gates.rs` 规则为函数指针表（Gate A 四条、Gate B 九条、改单闸五条），`RiskEngine` 最后消耗限速额度；`classify` 按仓位把新单归为 `Open`/`Reduce`。
> - `strategy`：`Strategy` trait（全部回调有默认空实现，`has_state/save_state/load_state` 描述自身快照）、`Context`、`Kernel`（instrument 表、订阅矩阵、簿、bar 聚合器、特征、定时器、Conflated/OnBatch 缓冲、输出）、`Trading`（OMS、ClientOrderId 生成器、Portfolio、RiskEngine、命令链 submit/modify/cancel/cancel_all/kill_switch、venue 事件入账与仓位事件、查询视图）。订单簿在 `on_book` 回调期间借给策略（此时 `ctx.book` 为 `None`）。
> - `engine`：`Engine::step(key, event)` 按固定顺序投递（订阅者 → 特征 → bar），`Every/Sampled/Conflated/OnBatch` 四种节奏，缓冲满时提前投递；内核事件（Submitted/Denied/Pending*、仓位事件）在回调返回后按产生顺序投递，投递中新产生的也在同一步处理；回调失败按 `ErrorPolicy` 停用策略并撤其全部订单；`NodeLifecycle` 驱动 `on_start`/`on_stop` 与 TradingState 的同步/降级保持；`lifecycle.rs` 是 §4.4 的 `next_state`；快照 = 内核状态 + 各策略自述状态。
> - `backtest`：`SimulatedExchange`（`TopOfBook`/`QueuePosition` 成交模型、GTX -5022、IOC/FOK 余量过期、STP 三模式、GTD 到期、-2011/-2013/-4028、reduce-only -2022、手续费、自己的 Portfolio）；`VenueLoop` 三条 FIFO 通道加 `JitteredLatency`，行情在 venue 时间撮合、`ts_init` 改写为 `+L_feed`，回报 `source_id = 0xFFFE`；`Driver` 合成生命周期、定时器、`BatchEnd`，`seq` 重新编号，输出紧随输入写入运行日志；`replay` 逐输入重放并比较输出（`ReplayDivergence`）；`MergeSource`/`ReplaySource`/`VecSource`。
> - `testkit`：`behaviour.rs` 读取 `tests/trace/behaviours/*.txt`，`Replayer` trait 与 `replay` 驱动正向 trace validation（首处偏差报行为号、步号、动作；规约动作从未出现也失败），`spec_tuples` 解析规约里的表。`corpus::fixtures` 提供各层测试共用的 instrument、账户与 17 种订单事件。
>
> 验收结果：execution 5 个测试（转移表 = 规约、OrderLifecycle 80 条行为逐步回放且规约未启用的事件被拒绝、venue 路径、淘汰、快照往返），risk 5 个（触发表与矩阵 = 规约、TradingState 40 条行为回放且不可接纳的命令被拒、规则原因码目录、结构检查、快照），engine 5 个（trade → 策略 → 下单 → 成交 → 仓位事件、四种节奏、错误策略、拒单输出、快照后两个引擎逐步输出与状态一致），backtest 5 个（Matching 行为回放、Binance 式回报、合并源、生命周期表、整段回测经 venue loop 运行后运行日志逐字节回放成功、同种子同指纹、不同种子不同指纹、被篡改配置报 `ReplayDivergence`），三个 profile 全部通过；十个内核 crate 与 corpus 在 `x86_64-unknown-none` 上编译；release 与 det-o0 的 200k corpus 逐字节一致并匹配 golden。`just rust-nostd` 与 CI 的 no_std 步骤原来按 `jarvis-` 前缀筛选 crate，改名后筛不到任何 crate，已改为显式列出。
>
> 未做（R3 第二阶段）：执行算法 `PeggedQuote`/`PassiveThenAggressive`（§11.4）、对账（§15，`Reconciliation` 规约）、事后监控与 KillSwitch 定时续期（§10.3、§10.5；`Shutdown` 的 KillSwitch 路径已有）、`StrategyError` 输入与 `RateLimitFeedback` 的 kind 字段、`NodeLifecycle`/`DepthSync`/`Reconciliation` 三个规约的正向 trace validation（分别依赖 node、adapter 与对账层）、零分配门覆盖每个 `step`（`VenueLoop` 目前按值克隆延迟事件）、criterion 基准。

- [x] task: execution — `OrderState`、订单状态机转移表（由规约核对）、`Oms`、`apply_order_event`（§8）
- [ ] task: execution — 执行算法 `PeggedQuote`、`PassiveThenAggressive`、`AlgoState` 竞技场、令牌预算（§11.4）
- [ ] task: execution — 对账：`VenueSnapshot`、`ReconciliationDiff`、`ReconcileOutcome`（§15）
- [x] task: risk — 规则目录、Gate A 与 Gate B、预留敞口、`TradingState`、固定窗口限速（§9、§10）；事后监控顺延到第二阶段
- [x] task: strategy — `Strategy` trait、`Context`、`Kernel` 与 `Trading` 服务；策略集合为 `Vec<Box<dyn Strategy>>`（静态泛型集合待基准证明需要再做）
- [x] task: engine — `Engine`、`step(key, e)` 与 `outputs()`、`lifecycle::next_state`、快照保存与恢复；`EventSource`/`Recorder` trait 在 backtest
- [x] task: backtest — `ReplaySource`/`MergeSource`、`VenueLoop`、`SimulatedExchange`、双时间线、成交模型枚举、`Driver`、`replay`（§12）
- [x] task: specs — 动作映射写在各 crate 的 trace 测试里（`testkit::behaviour`）；`tests/trace/behaviours/{OrderLifecycle,TradingState,Matching}.txt` 由 Rust 驱动运行
- [ ] task: harness — 零分配门覆盖每个 `step`；对应层测试移植；`step/trade_to_strategy` 基准
- [x] task: 验收 — 订单生命周期、撮合、风控的测试通过；快照保存与恢复逐字节往返；运行日志逐字节回放；三个规约（OrderLifecycle、TradingState、Matching）的正向 trace validation 通过，其余四个随其层到来

## R4 node、sys、Python 绑定（Python 示例零改动运行）

- [ ] task: sys — 崩溃安全的文件写入、阻塞 socket、共享库加载、停止信号、进程与环境变量、单调时钟（§3 sys）
- [ ] task: node — `NodeConfig`：toml + serde、`deny_unknown_fields`、`--env` 与 `--set` 覆盖、规范化与 hash 与 C++ 一致（§4.2）
- [ ] task: node — Node 生命周期状态机、`BacktestNode`、构建信息、`jarvis-rs replay` / `config` / `report`
- [ ] task: node — 快照文件与恢复（§16.3）
- [ ] task: python — `py` crate（PyO3 + maturin，abi3）：绑定全部模型类型、`PyStrategyHost`、`on_batch` 的只读 ndarray 视图、`jarvis.log` 读写；`python/jarvis/` 包按构建选项加载 C++ 或 Rust 扩展
- [ ] task: harness — `python/tests/` 对 Rust 扩展全部通过；`tests/golden/node_config` 通过
- [ ] task: python — `jarvis.log` 的纯 Python 读取器读 Rust 事件日志（线格式见 `model::log`），指纹与 `jarvis-rs fingerprint` 一致
- [ ] task: 验收 — `examples/py/` 零改动运行；回放 Rust 节点自己的日志逐字节复现输出

## R5 network、adapter、live（脚本化服务端与混沌测试通过）

- [ ] task: network — tokio + rustls + fastwebsockets 的 `WsClient`、hyper 的 `HttpsClient`、`Signer`；读空闲期限与 ping（§13）
- [ ] task: binance — `JsonCodec`（sonic-rs 或 simd-json）、流映射、订单簿同步、WS API、用户数据流、REST、启动检查、限速（§14）
- [ ] task: live — 环：热路径三条用 rtrb 传 `Event` / `QueuedCommand`，persist 用有界队列，冷路径 std mpsc；`Waker`；绑核（rust-migration.md §6.4）
- [ ] task: live — `MarketFeed`、`VenueIo`、`OrderTracker`、`Persister`、telemetry、admin、健康检查、`SandboxNode`、`LiveNode`（§7.1、§19）
- [ ] task: live — 原始帧录制与 `jarvis-capture redecode`（§13.4）
- [ ] task: harness — loom 覆盖环（若自写）与 `Waker` 的交换协议；miri 覆盖全部 `unsafe`；脚本化 HTTPS / WSS 服务端移植；混沌测试与延迟基准移植
- [ ] task: 验收 — 混沌测试两个种子通过；`redecode --check` 对自己录制的原始帧产出与运行日志一致的解码日志；sandbox 录制日志回放逐字节复现输出

## R6 切换

- [ ] task: python — wheel 默认加载 Rust 扩展；C++ 扩展作为 `JARVIS_IMPL=cpp` 的回退保留一个版本
- [ ] task: build — 删除 C++ 树、CMake、CPM、clang 配置；`just check` 的各层改为 Rust 配方；CI 作业图改为 lint → rust → {determinism, bench-compare, formal} → gate
- [ ] task: docs — architecture.md 的"执行手段"改为 Rust 机制；D12、D18 改写；cpp-subset.md 退役，替换为 Rust 树的 lint 清单
- [ ] task: 验收 — testnet 72 小时验收在 Rust 节点上重复一次
