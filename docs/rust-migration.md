# Rust 迁移方案

- 状态：Proposed
- 日期：2026-10-08
- 范围：把 `jarvis` 的 C++20 内核与 shell 迁移到 Rust 的收益、成本、技术栈、线程模型与迁移步骤
- 不改变：[architecture.md](architecture.md) 中 D01 到 D24 的决策（D12、D18 除外，见第 5 节）、事件日志线格式、Python 包 API、TLA+ 规约
- 前置：M5 的 testnet 验收（72 小时、混沌、反向 trace validation）先完成，再决定是否启动
- 实施计划：[rust-plan.md](rust-plan.md)（R0 到 R6）；代码在 `rust/`

本文回答三个问题：迁移到 Rust 能得到什么，技术栈怎么选，怎么迁。它是一份方案，不是决定。第 9 节给出决定的判据和一个两周试点。

## 目录

1. [结论](#1-结论)
2. [现状](#2-现状)
3. [收益：约束从门禁搬进编译器](#3-收益约束从门禁搬进编译器)
4. [得不到的东西与成本](#4-得不到的东西与成本)
5. [技术栈](#5-技术栈)
6. [线程拓扑与环](#6-线程拓扑与环)
7. [迁移策略](#7-迁移策略)
8. [门禁的对应关系](#8-门禁的对应关系)
9. [决定判据与试点](#9-决定判据与试点)
10. [开放问题](#10-开放问题)

---

## 1. 结论

1. 这个项目的架构已经是 Rust 形状的。`cpp-subset.md` 和 ADR 0001 禁掉的东西（异常、RTTI、`shared_ptr`、裸 `new`、浮点、有状态随机数、无序容器迭代、跨线程共享可变状态），恰好是 Rust 默认没有、或者一个 lint 就能禁掉的东西。迁移的主要收益是把现在用四个脚本和门禁维持的约束，变成编译器拒绝的东西。
2. 延迟不会变好。`step` 30 ns、L2 更新 50 ns、Python 回调 0.21 µs 这些数字，Rust 做到同一水平，不会更低。把迁移当作性能项目是错误预期。
3. 生态收益比语言本身大：nautilus 的领域模型是 Rust；Arrow/Parquet 可以进 shell；网络栈不必自写帧层；构建系统从 CMake + CPM + 工具链文件变成一个 Cargo workspace；Python wheel 由 maturin 产出。
4. 迁移方式是翻译，不是重新设计。现有 C++ 实现是 oracle：只要事件日志线格式、指纹算法和配置 hash 保持兼容，`tests/golden/` 与 corpus 指纹直接成为跨实现的等价测试，每一层的"移植完成"有机器判定的标准。
5. 如果项目目标是把 v1.0 发出去，现在不是时机。如果有 M6、M7 的长期计划，Rust 的收益随代码量和贡献者数放大，而且 nautilus 本身在 Rust 上这一点会越来越重要。

## 2. 现状

| 项目 | 数值 |
| --- | --- |
| C++ 源码（`jarvis/`、`python/src/`） | 约 52k 行，158 个头文件，127 个源文件 |
| Python 包（`python/jarvis/`） | 约 2k 行 |
| TLA+ 规约 | 7 个（OrderLifecycle、TradingState、Matching、Reconciliation、DepthSync、NodeLifecycle、SeqOrder），`specs/map/*.hpp` 把规约动作映射到内核事件 |
| 里程碑 | M0 到 M5 完成；M5 的 testnet 验收待做 |
| 门禁 | 分层检查、freestanding 编译、零分配门、Release 对 `-O0` 逐字节指纹、A/B 基准、golden、fuzz、TSan、混沌、形式化 |
| 构建 | CMake 3.25、CMakePresets、CPM 钉提交；Linux、macOS、Windows（clang-cl 或 MinGW，因为 `__int128` 排除了 MSVC） |

## 3. 收益：约束从门禁搬进编译器

| 现在的约束与执行手段 | Rust 中的对应 |
| --- | --- |
| 内核禁异常、禁 RTTI，靠 `jarvis_kernel_freestanding` 每个头文件单独以 `-fno-exceptions -fno-rtti` 编译 | 内核 crate 标 `#![no_std]`，错误就是 `Result<T, Status>`，不存在异常 |
| 内核禁 include 线程、时钟、随机数、流、第三方库，靠 `tools/check-layering.py` | `no_std` crate 没有这些模块；分层靠 Cargo workspace 的依赖图，不能反向依赖，不需要脚本 |
| "一个 owner 存 vector，其他人借 span"，靠评审 | 借用检查器就是这条规则 |
| 禁 `shared_ptr`、禁裸 `new/delete`，竞技场 + 代际句柄 | 内核 `#![forbid(unsafe_code)]`；slotmap 式竞技场是惯用写法 |
| 单写者：只有 core 线程写 `EngineState`，靠约定 + TSan preset | `Send`/`Sync` 在编译期拒绝跨线程共享可变状态；`EngineState` 由 core 线程的闭包独占，`!Send` 把规则变成类型 |
| 封闭 `std::variant` + `std::visit`，订单状态机转移表 | `enum` + `match` 穷尽检查，漏一个状态转移编译不过 |
| 有符号溢出是 UB，靠 `-Wconversion` 和 fuzz | 有符号溢出有定义；内核 profile 开 `overflow-checks`，clippy `arithmetic_side_effects` 强制显式 `checked_*`/`wrapping_*` |
| 禁浮点，靠评审 | clippy `float_arithmetic` 直接拒绝 |
| `__int128` 导致 MSVC 不支持 | `i128` 是原生类型，所有 target 一致，Windows 不再需要 clang-cl 或 MinGW |
| 两套编译器 × 两个优化级别逐字节对比 | 单一编译器，对比面缩小到 opt-level 0 对 3；这道门仍保留 |
| nautilus 兼容契约靠 `tests/conformance/nautilus_cd417b80.json` 逐项核对 | nautilus `crates/model` 本身是 Rust，`Price { raw: i64, precision: u8 }` 等类型逐字段照抄或直接依赖，核对面缩小到有意差异（architecture.md 第 6.8 节） |

生态上的收益：

- **Parquet 进 shell。** D18 把 Arrow 排除在内核构建外是因为 Arrow C++ 太重；arrow-rs 与 parquet crate 是纯 Rust，`jarvis.data` 的转换器可以下沉，Python 不再是唯一入口。
- **网络栈不必自写。** 第 13.1 节评分 24 分的方案是 Asio + OpenSSL + 自写 450 行 RFC 6455 帧层 + picohttpparser。tokio + rustls + fastwebsockets 覆盖同样需求，零拷贝接收路径是现成的。
- **构建与发布。** 16 KB 的 CMakeLists、CMakePresets、CPM、MinGW 工具链文件、clang-tidy 配置，换成一个 workspace 的 `Cargo.toml` 与 `clippy.toml`。maturin 产出 abi3 wheel，一个 wheel 覆盖 Python 3.11 以上全部版本。
- **测试工具链。** cargo-fuzz 替代 fuzz preset，proptest 替代 `testkit::Gen`，criterion 替代 google benchmark，miri 覆盖一部分 sanitizer 的工作，loom 对环做穷举（第 6 节）。

## 4. 得不到的东西与成本

- **延迟不变。** 第 7.8 节的实测值是单线程整数运算与一次 GIL 获取的成本，与语言无关。PyO3 与 nanobind 同量级，nanobind 通常略快。
- **零分配不是免费的。** Rust 的 `Vec` 一样会增长。固定容量容器、计数全局分配器、零分配门这套机制要原样重做；`#[global_allocator]` 只是让计数器更好写。
- **确定性有新陷阱。** `HashMap` 默认随机种子，`sort_unstable` 不稳定。两者都要用 clippy `disallowed_types`/`disallowed_methods` 在内核禁掉，只允许 `BTreeMap` 与稳定排序。
- **SBE 生态不成熟。** Spot 的 SBE codec（D14）需要自写生成器或补齐现有 crate，要留预算。
- **编译时间。** 泛型内核加 tokio shell 的全量构建比现在的 header-only C++ 慢，增量构建相当。
- **成本。** 52k 行、7 个规约的 trace 映射、已完成的 M5。单人估 3 到 6 个月，期间功能停滞。两棵树并行期间，每个 bug 修两次。

## 5. 技术栈

| 层 | 选择 | 说明 |
| --- | --- | --- |
| 内核十层（core 到 backtest） | 每层一个 crate，`#![no_std]` + `alloc`，`#![forbid(unsafe_code)]`，`#![deny(clippy::float_arithmetic, clippy::arithmetic_side_effects)]` | Cargo 依赖图替代 `check-layering.py`；`data` 与 `cost` 同级不互相依赖 |
| 错误模型 | `Result<T, Status>`，`Status` 是 `#[must_use]` 的 `enum` | 内核不用 anyhow/thiserror；shell 用 thiserror，跨入内核前翻译 |
| 容器 | 自写 `FixedVec<T, N>` 或 heapless；自写带代际的 32 位句柄竞技场 | 自写约 200 行，为了控制内存布局与 `CapacityExceeded` 语义 |
| 定点数值 | 照抄 nautilus 的 `Price`/`Quantity`/`Money`，乘法走 `i128` 并向零截断 | 试点期评估直接依赖 nautilus-model crate 的可行性（版本钉死、`no_std` 兼容性） |
| 随机数 | 自写 splitmix64 / Philox 计数器生成器 | 与现在一致，不用 rand crate 的有状态生成器 |
| 事件日志 | 手写定宽编码，**线格式、CRC-32C、段滚动与现有 C++ 日志逐字节相同** | 整个迁移的验收基础，见第 7 节 |
| 配置 | toml + serde，`#[serde(deny_unknown_fields)]` | 未知键报错免费获得；规范化与 hash 算法必须与 C++ 一致，否则日志头不兼容 |
| sys | std + libc / windows-sys，`File::sync_data` 做 fdatasync | 平台差异仍封在这一层 |
| network | tokio + rustls + fastwebsockets；REST 用 hyper | 异步只在 shell，内核保持同步；TLS 行为若需与 OpenSSL 一致可换 openssl crate |
| codec | sonic-rs 或 simd-json 替代 simdjson；SBE 自写生成器 | 见第 4 节 |
| 环与线程 | 热路径 rtrb；persist 用 rtrb 或 crossbeam `ArrayQueue`；冷路径 std mpsc | 见第 6 节 |
| Python | PyO3 + maturin，numpy crate 提供 `on_batch` 的只读 ndarray 视图 | `python/jarvis/` 包 API 不变，`examples/py/` 零改动 |
| 测试 | proptest、cargo-fuzz、criterion、loom、miri、cargo-nextest | golden 继续用 `tests/golden/` |
| 形式化 | TLA+ 规约不动，`specs/map/` 改为 Rust 模块 | trace validation 双向机制保留 |
| CI | clippy `-D warnings`、cargo-deny、opt-level 0 对 3 指纹门、A/B 基准 | 门禁结构不变，见第 8 节 |

决策记录中受影响的两条：D12（网络栈）改为 tokio 生态；D18（Parquet 只在 Python）改为 Parquet 可进 shell。其余不变。

## 6. 线程拓扑与环

### 6.1 为什么需要

第 7.1 节的拓扑是：core 线程是 `EngineState` 唯一写者，md-io、venue-io、REST、persist、telemetry 各自一个线程，彼此只通过 SPSC 环交换数据。

| 需求 | 没有环与线程会怎样 | 环与线程如何解决 |
| --- | --- | --- |
| 延迟目标：`step` p99 低于 20 µs，tick 到 socket p99 低于 200 µs | TLS 解密、WebSocket 解帧、一条几十 KB depth 快照的 JSON 解析都要几十 µs，全落在 `step` 前面 | IO 线程做完解码，环里传归一化定点事件 |
| 确定性：实盘全序是 core 的摄取顺序（D02） | 多个线程直接写状态，没有"第几个到"，日志无法回放 | 多个 IO 线程并行到达的东西，只在 core 从环取出时被赋 `seq` 并记录；这是非确定性进入确定性世界的唯一入口 |
| WAL | `fdatasync` 在 core 上，一次就是百微秒到毫秒 | persist 线程配 64 MiB 环；`async` 下 core 从不等待，`barrier` 下由 venue-io 等 durable 位置 |
| Python | IO 线程碰 Python 要抢 GIL；Python 回调慢时积压无处放 | 只有 core 持 GIL；环就是积压缓冲，满了按环各自的策略处理 |
| 排空优先级 `admin > 回执 > 行情 > timer`（第 5.5 节） | 单个多生产者队列里各来源的交错由谁先抢到 CAS 决定 | 每个来源一条环，core 按固定顺序排空 |

SPSC 而不是 MPSC：单生产者只要两个下标的普通读写加 acquire/release，多生产者要在 `head` 上 CAS；每个来源独立一条环才有排空顺序可言。

### 6.2 为什么是 unsafe

安全 Rust 的规则是同一块内存同一时刻要么一个可变引用、要么若干只读引用，跨线程共享还要 `Sync`。SPSC 环的正确性不是这两条能表达的：生产者在写 `slots[head & mask]`，消费者同时在读 `slots[tail & mask]`，安全的原因是"协议保证两个下标指向不同槽位，且 release/acquire 把写入顺序传过去了"。借用检查器不理解 head 与 tail 的算术关系，也不验证内存序。所以槽位要放在 `UnsafeCell` 后面，未初始化槽位要 `MaybeUninit`，拆出的 `Producer`/`Consumer` 要手写 `unsafe impl Send`，字节环返回的切片构造要指针运算。原子变量的 `load`/`store` 是安全 API，但 relaxed 还是 acquire 选错不会编译失败，只会在特定机器上偶发丢数据。

反过来，这也是 Rust 在这一块的收益：C++ 版本没有任何东西阻止两个线程对同一个环调用 `try_push`，单生产者只是约定。Rust 的 `Producer` 实现 `Send` 但不实现 `Sync` 和 `Clone`，只能被移动到一个线程，第二个线程想推入在编译期就被拒绝。

### 6.3 nautilus 的做法

以下基于对 nautilus 代码库的了解，不来自本仓库文档；细节以钉的提交 `cd417b80` 为准，入口是 `crates/live` 的 runner 与 `crates/common` 的 msgbus。

- **v1（Python/Cython）**：网络客户端是 Rust（tokio）经 PyO3 暴露，tokio 线程持 GIL 调 Python handler，handler 把消息放进 `LiveDataEngine`/`LiveExecutionEngine` 的 `asyncio.Queue`，主线程事件循环取出后经 `MessageBus` 同步派发。引擎、`Cache`、策略全部在主线程。队列有容量上限，满了记日志丢弃。
- **v2（Rust）**：数据客户端在 tokio 运行时上，产出的 `DataEvent` 经 `tokio::sync::mpsc` 无界通道发给 `AsyncRunner`；runner 单线程 `select!` 数据、时间事件与停止信号，经 msgbus 按字符串 topic 派发。引擎内部用 `Rc<RefCell<..>>`，整个引擎 `!Send`，这就是它的单写者。

| | nautilus v2 | jarvis |
| --- | --- | --- |
| 交接原语 | tokio 无界 mpsc：多生产者 CAS，按块分配 | SPSC 环：定长，启动时分配，之后零分配 |
| 背压 | 无界，内存增长 | 定长，满了丢弃计数或反压，由环决定 |
| 交接点语义 | 只是传递 | 赋 `seq` 并写 WAL，是全序与回放的定义点 |
| 派发 | 字符串 topic + 通配匹配 | 封闭 variant + 订阅矩阵 |
| 实盘回放 | 不以逐字节回放为目标 | 回放摄取日志逐字节一致是验收项 |

两边都在"IO 并行、引擎单线程"上收敛，LMAX Disruptor 与 Aeron 也是同一结构。nautilus 没有把交接点当记录点，是因为它的目标里没有实盘逐字节回放。

### 6.4 备选方案与选择

| 方案 | 评价 | 结论 |
| --- | --- | --- |
| rtrb 替代手写环 | 设计与 `SpscRing<T>` 几乎相同（缓存对方下标、缓存行填充、wait-free），API 全部安全，unsafe 在库内且被审过。环里传封闭 `Event` 枚举而不是编码字节，`SpscByteRing` 不再需要；depth 快照这类大变长记录走 IO 线程的 slab 加句柄，不撑大枚举。基准上 690 ns 的往返几乎全是跨核唤醒与缓存同步，换库不会变慢 | **采用** |
| 一条环多消费者（Disruptor 式） | core 与 persist 读同一份数据，省一次拷贝。但 persist 写的是 core 赋 `seq` 后的合并顺序，只有 core 知道，这次拷贝是语义必需 | 不采用 |
| mmap 文件作为环，环即 WAL（Chronicle、Aeron 日志缓冲） | persist 线程消失，外部进程可 tail 文件。msync 的持久化语义比 `fdatasync` 难控制，Windows 差异大，unsafe 更多。收益在多进程隔离，而 jarvis 的分片是每节点一进程 | 不采用 |
| 全部单线程 + io_uring/epoll | 最简单，无环无 unsafe。TLS、JSON、`fdatasync` 全落在 `step` 路径，20 µs 的 p99 做不到 | 不采用；延迟目标放宽到毫秒时才成立 |
| tokio mpsc / std mpsc | admin、telemetry 这类冷路径够用 | 冷路径采用 |

具体接线：行情入、命令出、回执入三条热路径用 rtrb 传 `Event`/`QueuedCommand`；persist 环用 rtrb 或 crossbeam 有界 `ArrayQueue`（需要背压与 `stalls` 计数）；admin 与 telemetry 用 std mpsc；`Waker` 仍是 eventfd/pipe/事件对象加一个原子标志交换。有 `unsafe` 的地方只剩 libc 的绑核与 eventfd 调用，以及 PyO3 生成代码内部。

## 7. 迁移策略

1. **翻译而不是重新设计。** `step(S, e) → (S′, out[])`、单写者、三环境只换三条边、两道风控闸、WAL 即 trace，全部保留。architecture.md 几乎不动，变的是"执行手段"一列。
2. **现有 C++ 实现是 oracle，逐字节验收。** 事件日志线格式、`jarvis fingerprint` 算法、配置规范化 hash 三样保持兼容，于是 `tests/golden/*/expected.sha256`、seed 7 的 20 万条语料、M2 一天 BTCUSDT 数据的运行日志都成为跨实现等价测试。自底向上移植：core 与 model 过 corpus 指纹，engine 与 backtest 过 golden 回放，再往上。每一层的"完成"有机器判定标准。
3. **两棵树，不混一个二进制。** cxx 对 header-only 的 concept 模板几乎没有办法，桥接代码会比被桥接的多。两棵树并行，共享数据与 golden，Python 包按构建选项加载哪个扩展模块，Rust 树全部通过后切换。
4. **先内核后 shell。** 内核有 oracle，收益是编译期保证；shell 的收益是生态替换，但 shell 的验收（testnet 72 小时）尚未完成，先移植它等于同时调两个未知量。
5. **反惯用写进 lint，不写进文档。** 禁 `HashMap`、禁浮点、禁 `sort_unstable`、禁 `std::time` 进内核，全部用 `clippy.toml` 与 `no_std` 落地。

移植顺序与每步的验收：

| 步 | 范围 | 验收 |
| --- | --- | --- |
| 1 | core、model、日志编码 | `jarvis corpus --seed 7 --events 200000` 的 Rust 实现与 C++ 输出逐字节相同；`tests/golden/model_strings`、`corpus_seed42` 通过 |
| 2 | data、cost、portfolio | `replay_quote`、`replay_trade`、`replay_book`、`replay_bar`、`replay_feature` golden 通过 |
| 3 | execution、risk、strategy、engine、backtest | `replay_orders`、`replay_batch`、`snapshot_orders`、`example_*` golden 通过；`specs/map/` 改为 Rust，`tests/trace/` 的行为文件通过 |
| 4 | node、sys、Python 绑定 | `python/tests/` 全部通过，`examples/py/` 零改动运行，运行日志与 C++ 树逐字节相同 |
| 5 | network、adapter、live | 对 `tests/cpp/support` 的脚本化服务端与混沌测试通过；`jarvis-capture redecode` 对同一原始帧文件产出与 C++ 相同的解码日志 |
| 6 | 切换 | 删除 C++ 树；`just check` 全绿 |

## 8. 门禁的对应关系

| 现在 | Rust 树 |
| --- | --- |
| `tools/check-layering.py` | Cargo 依赖图；额外用 cargo-deny 禁止内核 crate 依赖 std 或任何第三方 |
| `jarvis_kernel_freestanding` | 内核 crate `#![no_std]`，CI 以 `--target` 一个无 std 的目标编译一次 |
| 零分配门（计数 `operator new`） | `#[global_allocator]` 计数分配器，测试中 `step` 期间计数必须为零 |
| Release 对 `-O0` 指纹 | opt-level 3 对 opt-level 0 指纹，另加一个 stable 与一个 nightly 工具链 |
| clang-tidy、`-Wconversion` | clippy `-D warnings`，`clippy.toml` 配 `disallowed_types`/`disallowed_methods` |
| TSan preset | 环 crate 用 loom 穷举交错，unsafe 代码用 miri；shell 整体仍可跑 TSan（`-Zsanitizer=thread`，nightly） |
| fuzz preset | cargo-fuzz，语料目录 `tests/fuzz/corpus/` 复用 |
| google benchmark + `bench_compare.py` | criterion 输出 JSON，`bench_compare.py` 读新格式；阈值文件不变 |
| golden | 不变 |
| TLA+ 与 trace validation | 规约不变；`specs/map/` 改为 Rust 模块，`tests/trace/` 的行为文件复用 |
| 混沌、soak、延迟基准 | 不变，对同一套脚本化服务端运行 |

## 9. 决定判据与试点

启动迁移的条件，全部满足才开始：

1. M5 验收完成，v1.0 已发布或已明确推迟。
2. 有 M6 或 M7 的明确计划，且其中至少两项（Spot SBE、hedge 模式、io_uring、分片、free-threading）会显著增加 shell 代码量。
3. 第 10 节的开放问题中，nautilus-model crate 依赖可行性与 SBE 生成器两项有结论。
4. 下面的试点通过。

两周试点，不写生产代码：

| 周 | 内容 | 通过标准 |
| --- | --- | --- |
| 1 | 写 Rust 树的约束文件（本文第 3、5 节每条约束对应的 lint 或 crate 属性），搭 workspace 骨架、clippy.toml、cargo-deny 配置、计数分配器、指纹门 CI | 故意注入一次 `HashMap`、一次 `f64`、一次 `step` 内分配、一次跨线程 `&mut`，四道门各自报错 |
| 2 | 移植 core 层与日志编码，对拍 corpus | seed 7 的 20 万条语料逐字节相同；`log/append_record`、`log/decode_record` 基准与 C++ 的 89 ns、75 ns 同量级 |

试点失败（任一标准不满足，或 core 层移植超过两周）即停止，本文保持 Proposed 并记录原因。

## 10. 开放问题

| 问题 | 推荐默认 | 何时决定 |
| --- | --- | --- |
| 直接依赖 nautilus-model crate，还是照抄类型 | 照抄。nautilus 的 crate 非 `no_std`，且带 PyO3 特性门；照抄约 2k 行，核对测试复用 | 试点第 1 周 |
| `Event` 枚举过环时 depth 快照的传递方式 | IO 线程 slab + 代际句柄，core 读后释放 | 第 5 步之前 |
| SBE 生成器 | 自写最小生成器，只覆盖 Binance Spot schema 用到的子集 | M6 开始前 |
| TLS 库 | rustls；若 testnet 对拍发现握手或重连行为差异再换 openssl crate | 第 5 步 |
| PyO3 abi3 还是按版本编译 | abi3（3.11 下限）；若 `on_batch` 的 ndarray 视图在 abi3 下有性能损失再评估 | 第 4 步 |
| nightly 依赖 | 不依赖；TSan 作为可选 nightly job，stable 门禁不含它 | 试点第 1 周 |
| 两棵树并行期的 bug 修复策略 | 先修 C++（生产），Rust 树按步骤到达该层时同步；用 golden 捕获 | 启动时 |
