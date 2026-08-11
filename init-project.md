

## 0. 你要造什么

一个 **C++20 编写的确定性、事件驱动回测内核**,通过 nanobind 暴露给 Python 策略。
Bar 级先跑通,tick(quote/trade 双流)在同一套机制上,不做特例。



### 非目标

- 不做单次回测内部的多线程 / Time Warp 乐观并行
- 内核里不写 SIMD intrinsics(状态机不可向量化,这是结论不是待办)
- 不做策略库、不做数据下载、不做可视化
- 不追求"支持所有订单类型",先做对市价+限价

---

## 1. 确定性硬约束 [HARD]

违反任何一条,整个项目失去意义。这五条要写进 `docs/adr/0001` 并在 CI 强制。

1. **全整数**。价格 = tick 数(int64),数量 = lot 数(int64),现金 = `__int128`。
   状态机中**禁止出现任何浮点**。浮点只允许出现在最终指标展示层。
2. **严格全序**。事件键 `(ts, source_id, row)` 三元组,不允许并列。
   挂单键 `(ts_visible, order_id)`,order_id 单调唯一。
3. **纯函数转移**。`step(S, e)` 禁止读取:wall clock、全局 RNG 状态、指针地址、
   无序容器的迭代顺序、任何环境变量。
4. **counter-based RNG**。滑点/延迟抖动用 splitmix64/Philox,种子为 `(seed, order_id)`,
   不依赖调用顺序。禁止 `std::mt19937` 之类的有状态生成器。
5. **编译期无关**。`-ffp-contract=off -fno-fast-math`,禁止 `-march=native`。
   `-O0` 与 `-O3` 构建的输出指纹必须逐位相同。

---

## 2. C++ 子集规范 [HARD]

写进 `docs/cpp-subset.md`,并用编译器+clang-tidy 强制。

**禁用**:内核内异常、RTTI/`dynamic_cast`、热路径虚函数、继承复用实现(仅允许纯接口)、
`shared_ptr`、SFINAE/模板元编程、运算符重载(仅留比较与 `[]`)、隐式转换(构造函数一律 `explicit`)、
`iostream`、宏(仅留平台探测)、热路径的 `std::function`。

**允许**:`struct` + 成员函数 + RAII、`vector`/`array`/`span`/`optional`/`string_view`、
`enum class`、`constexpr`、`[[nodiscard]]`、结构化绑定、range-for、concept 约束的简单模板。

**Rust 心智映射**(团队背景是 Rust):

- 所有权 → 单一 owner 持 `vector`,别处传 `std::span`,禁裸 `new/delete`
- 生命周期 → **arena + 整数索引句柄**,不是指针。`OrderId` 是 `uint32`。
  这同时解决序列化、快照、ABA 三个问题
- `Result<T,E>` → 返回 `enum class Status` + out 参数(内核无异常)
- 默认不可变 → `const` 拧到底

---

## 3. 工具链 [HARD]

| 项       | 选择                                           | 说明                                                           |
| ------- | -------------------------------------------- | ------------------------------------------------------------ |
| 构建      | CMake ≥ 3.25 + **CMakePresets.json** + Ninja | flag 只有一份真相,全在 preset 里                                      |
| 依赖      | **CPM.cmake**,vendored 到 `cmake/`,所有依赖钉 tag  | 在 ADR 写明迁移触发器:引入 Arrow C++/TBB/Highway 任一 → 切 vcpkg manifest |
| 编译器     | GCC ≥13 / Clang ≥17 / AppleClang ≥15         | **不用 C++20 modules**,构建工具支持不稳                                |
| 测试      | **doctest**                                  | 编译最快。性质测试自带 splitmix64 生成器,不引 RapidCheck                     |
| 基准      | **google/benchmark**,JSON 输出                 | CI 存档,追踪回归                                                   |
| 绑定      | **nanobind**                                 | 不用 pybind11                                                  |
| 打包      | **scikit-build-core** + `pyproject.toml`     | `pip install .` 出 wheel                                      |
| 任务入口    | **justfile**                                 | `just test` / `just fp` / `just bench` / `just lint`         |
| 格式/静态检查 | clang-format + clang-tidy,接 pre-commit       |                                                              |

### 编译选项

```
公共:  -Wall -Wextra -Werror
       -Wconversion -Wsign-conversion -Wshadow -Wswitch-enum
       -Wnon-virtual-dtor -Wold-style-cast
       -ffp-contract=off -fno-fast-math
Debug: -D_GLIBCXX_ASSERTIONS -fno-omit-frame-pointer
       (macOS/libc++ 用 -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)
Sanitizer preset: -fsanitize=address,undefined
```

`-Wconversion -Wsign-conversion` 是核心,它把 Rust 的显式转换纪律搬过来。**不许关。**

### CMake target 划分 [HARD]

- `jarvis::strict` — INTERFACE target,承载全部编译约束,所有一方 target 挂它
- `jarvis::kernel` — INTERFACE,header-only 内核
- `jarvis_kernel_freestanding` — **守门 OBJECT target**,include 全部内核头,
  用 `-fno-exceptions -fno-rtti` 编译。谁往内核塞 `throw`,构建失败。
- `jarvis_tests` / `jarvis_bench` / `_core`(绑定,单独开异常做边界翻译)

### CMakePresets 必须包含

`dev`(Debug+ASan/UBSan)、`rel`(Release)、`det-o0`(Debug 但 `-O0`,用于指纹交叉)、`bench`。

### CI(GitHub Actions)三个 job

1. **test** — 矩阵 `{ubuntu-24.04 × gcc-13, ubuntu-24.04 × clang-18, macos-14 × AppleClang}`,
   全部在 ASan+UBSan 下跑 ctest + pytest
2. **determinism** — 构建 `rel` 和 `det-o0`;首个内核状态落地后启用逐字节指纹门
3. **bench** — main 分支出 JSON 存档,**先观测不设阈值**

---

## 4. 目录结构

```
jarvis/
  core/
  model/
  sim/
  data/
  engine/

python/
testkit/
```

---
