# 开发环境与日常工作流

本文讲如何在 Linux 与 macOS 上搭建开发环境、配置 VS Code 与 Neovim，以及编译、测试、运行的日常流程；Windows 见第 6 节。设计见 `docs/architecture.md`，里程碑见 `docs/plan.md`，运行节点见 `docs/runbook.md`。

## 1. 工具与目录

| 工具 | 版本 | 用途 |
| --- | --- | --- |
| CMake | ≥ 3.25 | 构建，配置写在 `CMakePresets.json` |
| Ninja | 任意 | preset 的生成器 |
| C++ 编译器 | GCC 13+、Clang 17+ 或 AppleClang 15+；Windows 上 clang-cl 17+ 或 MinGW-w64 GCC 13+ | C++20，用到 `__int128`，所以不支持 MSVC 的 cl.exe（第 6 节） |
| OpenSSL | 3.x，带头文件 | 网络层与 live shell；所有 preset 都打开 `JARVIS_BUILD_LIVE` |
| Python | ≥ 3.11 | 策略层、工具、pytest；由 `uv` 管理 |
| uv | 任意 | 创建 `.venv`、安装 Python 包 |
| just | 任意 | 命令入口（`justfile`） |
| clang-format、clang-tidy | 18 | 格式与静态检查；CI 用 18，别的版本格式可能不同 |
| clangd | 最好 18 | 编辑器的 C++ 语言服务 |
| Java | ≥ 17 | TLA+ 模型检查（TLC）；第一次运行时下载固定版本的 `tla2tools.jar` |
| gdb 或 lldb | 任意 | 调试 |

C++ 依赖（nanobind、doctest、google benchmark、toml++、Asio、picohttpparser、simdjson）由 CPM 在第一次 configure 时从 GitHub 下载，钉在 `cmake/Dependencies.cmake` 的提交上，不需要手工安装。

目录约定：

- `.venv/`：项目的 Python 环境，`just bootstrap` 创建。
- `build/<preset>/`：每个 preset 一个构建目录。可执行文件在 `build/<preset>/bin/`，测试在 `build/<preset>/tests/`，`compile_commands.json` 在构建目录根上。
- `build/wheel/`：Python 扩展模块的构建目录。
- `runs/`、`catalog/`：示例的运行目录与数据目录，已在 `.gitignore` 中，需要时自己清理。

## 2. 安装工具

### 2.1 Linux（Ubuntu 24.04）

24.04 默认的 LLVM 是 18，GCC 是 13，CMake 是 3.28：

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git curl \
    clang clang-format clang-tidy clangd libclang-rt-18-dev \
    libssl-dev openjdk-17-jre-headless gdb
curl -LsSf https://astral.sh/uv/install.sh | sh       # uv
uv tool install rust-just                              # just
clang-format --version                                 # 应为 18
```

- `libclang-rt-18-dev` 是 `tsan` 与 `fuzz` preset 需要的 Clang 运行时库；`fuzz` preset 用不带版本号的 `clang++`。
- 系统上默认的 clang-format 不是 18 时（例如另装了别的 LLVM），装带版本号的包并告诉脚本：
  ```sh
  # ~/.bashrc 或 ~/.zshrc
  export CLANG_FORMAT=clang-format-18
  export CLANG_TIDY=clang-tidy-18
  ```
- 更早的发行版（如 22.04）自带的 CMake 与 GCC 太旧：CMake 可用 `uv tool install cmake`，GCC 13 与 LLVM 18 用 ubuntu-toolchain PPA 与 apt.llvm.org。其他发行版装同样的工具即可。

### 2.2 macOS（14 及以后，Apple Silicon 或 Intel）

编译器用 Xcode 命令行工具里的 AppleClang，其余用 Homebrew：

```sh
xcode-select --install
brew install cmake ninja openssl@3 llvm@18 uv just openjdk@17 bash
```

在 `~/.zshrc` 中加：

```sh
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"     # CMake 找不到 Homebrew 的 OpenSSL
export CLANG_FORMAT="$(brew --prefix llvm@18)/bin/clang-format"
export CLANG_TIDY="$(brew --prefix llvm@18)/bin/clang-tidy"
export PATH="$(brew --prefix openjdk@17)/bin:$PATH"
```

- `llvm@18` 是 keg-only，不会替换系统的 `clang`。日常构建仍用 AppleClang（CI 的 macOS job 也是）；它只提供 clang-format、clang-tidy、clangd，以及 fuzz 需要的 libFuzzer（AppleClang 没有）。在 macOS 上构建 fuzz 目标要显式指定编译器，不能直接用 `just fuzz`：
  ```sh
  cmake --preset fuzz -DCMAKE_CXX_COMPILER="$(brew --prefix llvm@18)/bin/clang++"
  cmake --build --preset fuzz
  mkdir -p build/fuzz/corpus-scratch/ws    # 新样本写进第一个目录，入库的语料保持不变
  build/fuzz/tests/fuzz/fuzz_ws -max_total_time=60 build/fuzz/corpus-scratch/ws tests/fuzz/corpus/ws
  ```
- `bash`：macOS 自带 bash 3.2，`tools/bench_ab.sh` 需要 bash 4 以上；保证 Homebrew 的 bin 目录在 `PATH` 中排在 `/bin` 前面。
- 不支持的部分：`[threads]` 绑核只在 Linux 与 Windows 上可用。CI 在 macOS 上只跑 `dev` 的构建与测试；`tsan`、fuzz、lint 与确定性门只在 Linux 上跑，以 Linux 的结果为准。

### 2.3 Windows

见第 6.2 节。

### 2.4 可选：共享依赖缓存

每个构建目录默认各自下载一份 C++ 依赖。设置缓存目录后各 preset 共用一份，也避免重复下载：

```sh
export CPM_SOURCE_CACHE="$HOME/.cache/CPM"
```

Windows（PowerShell，用户级）：`[Environment]::SetEnvironmentVariable("CPM_SOURCE_CACHE", "$env:LOCALAPPDATA\CPM", "User")`。

## 3. 第一次构建

```sh
git clone git@github.com:nivalume/jarvistrader.git && cd jarvistrader
just bootstrap               # .venv（Python 3.11）、scikit-build-core、pytest、pre-commit
just build                   # configure + build 的 dev preset（Debug，ASan 与 UBSan）
ctest --preset dev           # C++ 测试，76 项
just develop                 # 可编辑安装 Python 包（带 live shell）
.venv/bin/python -m pytest   # Python 与工具测试
.venv/bin/pre-commit install # 每次提交前跑格式与静态检查
```

- `just build` 会把 `.venv` 的 Python 传给 CMake（`-DPython_EXECUTABLE`）。直接用 `cmake --preset dev` 时也要传，否则 CMake 可能找到别的 Python。
- 并行编译的进程多了会耗尽内存（ASan 构建尤其明显）。用 `CMAKE_BUILD_PARALLEL_LEVEL=4 just build` 或 `cmake --build --preset dev -j4` 限制。
- 第一次 configure 要访问 GitHub 下载依赖。

### 3.1 preset

| preset | 构建类型 | 用途 |
| --- | --- | --- |
| `dev` | Debug，ASan + UBSan | 日常开发与测试；编辑器读它的 `compile_commands.json` |
| `rel` | Release | 性能接近生产的运行、确定性门的一边 |
| `det-o0` | Debug，强制 `-O0` | 确定性门的另一边：与 `rel` 写出的事件日志必须逐字节相同 |
| `bench` | Release，只构建基准 | `just bench`、延迟基准 |
| `tsan` | Debug，ThreadSanitizer，不构建 Python | 多线程代码（环、IO 线程、live 节点） |
| `fuzz` | RelWithDebInfo，Clang + libFuzzer | 解析器与解码器的 fuzz 目标 |
| `win-dev`、`win-rel` | Debug、Release，clang-cl | Windows 上的日常构建（第 6 节），没有 sanitizer |
| `mingw-dev` | Debug，MinGW-w64 GCC，不构建 Python | 在 MSYS2 的 MINGW64 shell 里构建 |
| `mingw-cross` | Debug，MinGW-w64 交叉编译，不构建 Python | 在 Linux 上构建 Windows 程序，ctest 经 Wine 运行 |

```sh
just build rel                         # 或 cmake --preset rel && cmake --build --preset rel
cmake --build --preset dev --target test_engine   # 只构建一个目标
```

## 4. 编辑器

C++ 用 clangd，Python 用 Pylance 或 pyright。仓库根目录的 `.clangd` 让 clangd 读取 `build/dev/compile_commands.json`，所以先 `just configure`（或 `just build`）一次，编辑器才能正确解析头文件与宏。

格式：`.clang-format` 是唯一的格式定义。clangd 自带的格式化引擎跟着 clangd 版本走，不同版本的输出可能与 CI 的 clang-format 18 不同。所以要么用 clangd 18，要么关掉保存时格式化、在提交前跑 `just format`。

### 4.1 VS Code

扩展：

| 扩展 | 用途 |
| --- | --- |
| `llvm-vs-code-extensions.vscode-clangd` | C++ 补全、跳转、诊断、clang-tidy |
| `ms-vscode.cmake-tools` | 按 preset 配置与构建，CTest 集成 |
| `vadimcn.vscode-lldb`（CodeLLDB） | 调试 C++；macOS 与 Linux 都能用 |
| `matepek.vscode-catch2-test-adapter`（C++ TestMate） | 在测试面板里按 `TEST_CASE` 运行与调试 doctest |
| `ms-python.python`、`ms-python.debugpy` | Python 与调试 |
| `tamasfe.even-better-toml` | TOML 配置 |
| `alygin.vscode-tlaplus` | TLA+ 规约（可选） |

微软的 C/C++ 扩展（`ms-vscode.cpptools`）与 clangd 的功能重复，推荐列表把它列为不推荐；已经装了的，共享设置里关掉了它的 IntelliSense（`"C_Cpp.intelliSenseEngine": "disabled"`）。

`.vscode/` 里的 `settings.json`、`launch.json`、`tasks.json`、`extensions.json` 随仓库提交（`.gitignore` 只放行这四个文件，其余个人文件不入库）。Linux、macOS 与 Windows 共用同一份（Windows 的差别见第 6.3 节）；打开仓库后 VS Code 会提示安装推荐的扩展。个人偏好写在用户设置里，不要改这几个文件。

- `settings.json`：CMake Tools 只用 preset，并把 `.venv` 的 Python 传给 CMake；clangd 带 `--background-index --clang-tidy --header-insertion=never`；C++ 保存时用 clangd 格式化（clangd 不是 18 时关掉，见上）；Python 解释器与 pytest 指向 `.venv`；`build/dev/tests/test_*` 交给 TestMate 运行与调试；文件监视排除 `build/` 与 `runs/`。
- `launch.json`：三个调试配置。`C++: test_engine` 与 `C++: pegged_mm, first 10 minutes` 用 lldb（CodeLLDB）；`Python: mm_quote, first 10 minutes` 用 debugpy，解释器是 `.venv`，`justMyCode` 关闭，所以能进入 `jarvis` 包。需要别的测试或参数时改 `program` 与 `args`。
- `tasks.json`：`clean runs/mm-dbg`，是两个 mm 配置的 `preLaunchTask`。

使用时注意：

- **clangd 的路径不写在共享设置里。** Linux 用 `PATH` 上的 clangd（18）；macOS 在用户设置里写 `"clangd.path": "/opt/homebrew/opt/llvm@18/bin/clangd"`（Intel Mac 为 `/usr/local/opt/llvm@18/bin/clangd`）。
- **两个 mm 配置读 `catalog/`，要先按 5.3 节下载数据**，否则程序以退出码 1 结束。错误信息（`no data for ... NotFound`）在调试终端的输出里；调试器弹出的 `SystemExit: 1` 只表示进程以非零码退出。
- **两个 mm 配置写 `runs/mm-dbg`。** 程序不覆盖已有的运行日志（`AlreadyExists`，退出码 1），所以每次启动前由 `tasks.json` 清掉这个目录。要保留某次运行，改 `--out`。
- C++ 配置运行 `build/dev` 里的可执行文件，要先 `just build`。

LeakSanitizer 在调试器下（ptrace）不能工作，所以调试 `dev` 构建的程序时设 `ASAN_OPTIONS=detect_leaks=0`；ASan 的其他检查照常。

### 4.2 Neovim

需要 Neovim 0.11 或以后（下面用内置的 `vim.lsp.config` 与 `vim.lsp.enable`；0.10 用 nvim-lspconfig 的 `require("lspconfig").clangd.setup({...})`，参数相同）。语言服务：clangd，以及 pyright 或 basedpyright（`npm i -g pyright`，或用 mason.nvim 安装）。

`~/.config/nvim/after/plugin/jarvis.lua`（或放进你自己的配置）：

```lua
-- C++：.clangd 指向 build/dev 的编译数据库
vim.lsp.config("clangd", {
  cmd = { "clangd", "--background-index", "--clang-tidy", "--header-insertion=never" },
  -- macOS: cmd = { "/opt/homebrew/opt/llvm@18/bin/clangd", ... }
  filetypes = { "c", "cpp" },
  root_markers = { ".clangd", "compile_commands.json", ".git" },
})

-- Python：用项目的 .venv（pyright 从这个解释器找到编译出的 jarvis._core）
vim.lsp.config("pyright", {
  cmd = { "pyright-langserver", "--stdio" },
  filetypes = { "python" },
  root_markers = { "pyproject.toml", ".git" },
  settings = { python = {} },
  before_init = function(_, config)
    config.settings.python.pythonPath = config.root_dir .. "/.venv/bin/python"
  end,
})

vim.lsp.enable({ "clangd", "pyright" })

-- 构建错误进 quickfix：:make 等于 cmake --build --preset dev
vim.api.nvim_create_autocmd("FileType", {
  pattern = { "cpp", "c", "cmake" },
  callback = function() vim.opt_local.makeprg = "cmake --build --preset dev" end,
})
```

- 格式：clangd 18 的 `vim.lsp.buf.format()` 与 CI 一致；用 conform.nvim 时把 cpp 的 formatter 设为 clang-format（读取仓库的 `.clang-format`），命令指向 18 版。
- 跳转到头文件与源文件对：clangd 的 `textDocument/switchSourceHeader`（`:LspClangdSwitchSourceHeader`，nvim-lspconfig 提供）。
- 调试：nvim-dap 加 codelldb（mason 安装 `codelldb`）：

```lua
local dap = require("dap")
dap.adapters.codelldb = { type = "executable", command = "codelldb" }
dap.configurations.cpp = {
  {
    name = "test binary",
    type = "codelldb",
    request = "launch",
    program = function()
      return vim.fn.input("binary: ", vim.fn.getcwd() .. "/build/dev/tests/", "file")
    end,
    args = function() return { vim.fn.input("doctest args: ", "-tc=") } end,
    cwd = "${workspaceFolder}",
    env = { ASAN_OPTIONS = "detect_leaks=0" },
  },
}
```

- 跑测试：在终端（`:terminal` 或 tmux 另一个窗格）里用下一节的命令最直接；neotest 有 doctest 与 pytest 的适配器，可选。

## 5. 日常工作流

### 5.1 改 C++

增量构建一个测试目标，只跑相关的用例：

```sh
cmake --build --preset dev --target test_engine
build/dev/tests/test_engine -tc="*degrades*"                         # 按 TEST_CASE 名过滤
build/dev/tests/test_engine -ts=property -tc="*function of its inputs*" # 按 suite 与名字
build/dev/tests/test_engine -ltc                                      # 列出用例
```

- 每一层一个测试程序（`tests/cpp/test_<layer>.cpp`，登记在 `tests/CMakeLists.txt`）：`test_core`、`test_model`、`test_engine`、`test_execution`、`test_backtest`、`test_node`、`test_adapter`、`test_live`、`test_chaos` 等。
- doctest 常用参数：`-tc`（用例名，支持 `*`）、`-sc`（子用例）、`-ts`（suite）、`-s`（打印通过的断言）、`--abort-after=1`。
- 用 ctest 按名字或标签选：
  ```sh
  ctest --preset dev -R engine          # 名字含 engine
  ctest --preset dev -L unit            # 标签：unit、property、conformance、golden、zero-alloc、trace
  ctest --preset dev -L live -j4        # 也可按层：live、adapter、network、chaos ……
  ctest --preset dev                    # 全部
  ```
- 属性测试：失败信息里有种子与用例号。用环境变量重放一个用例：
  ```sh
  JARVIS_PROP_SEED=2 JARVIS_PROP_CASE=137 build/dev/tests/test_engine -ts=property \
      -tc="*function of its inputs*"
  ```
  `JARVIS_PROP_ITERS` 改用例数。ctest 用种子 1、2、3 各跑一遍（`engine.property.seed1` 等）。
- 新增测试：加在对应层的 `tests/cpp/test_<layer>.cpp`，用 `TEST_SUITE("unit")`（或 `property` 等）包住；新文件要加进 `tests/CMakeLists.txt` 里该层的 `SOURCES`。
- 写内核代码要守 `docs/cpp-subset.md`（不用异常、RTTI、热路径上的虚调用与内存分配）和分层规则（`tools/check-layering.py`，`just layering`）。

### 5.2 改 Python 或绑定

`just develop` 做的是可编辑安装：

- `python/jarvis/` 下的 Python 文件改了立即生效；
- C++ 改了（`jarvis/` 或 `python/src/` 的绑定），下一次 `import jarvis` 时扩展模块在 `build/wheel/` 中增量重建（Release 构建，几秒到几分钟）；
- `just install`、`just install-live`、`just test` 会换成普通安装，之后要再 `just develop` 一次。

```sh
.venv/bin/python -m pytest python/tests/test_node.py -k replay   # 一个文件里名字含 replay 的用例
.venv/bin/python -m pytest -x -q          # 全部，遇到第一个失败就停
```

带 live shell 的构建里，`python/tests/test_node.py` 有一项按设计跳过（它测的是不带 live 的构建）。

### 5.3 运行

`jarvis` 命令行（`build/<preset>/bin/jarvis`）：

```sh
build/dev/bin/jarvis config examples/config/node.toml --env sandbox   # 规范形式与配置 hash
build/dev/bin/jarvis dump runs/mm --limit 20                          # 事件日志转文本
build/dev/bin/jarvis report runs/mm                                   # 回测报告：成交、手续费、盈亏
build/dev/bin/jarvis snapshot "$(ls runs/mm/*.jsnap | tail -1)"      # 最后一个快照的摘要
```

回测示例需要先下载数据（data.binance.vision）到 catalog：

```sh
.venv/bin/python -m jarvis.data binance-vision aggTrades  --symbol BTCUSDT --start 2024-03-30 --catalog catalog
.venv/bin/python -m jarvis.data binance-vision bookTicker --symbol BTCUSDT --start 2024-03-30 --catalog catalog
.venv/bin/python -m jarvis.data binance-instrument --symbol BTCUSDT --day 2024-03-30 --catalog catalog \
    --tick 0.10 --step 0.001 --min-qty 0.001 --max-qty 1000 --min-notional 100

.venv/bin/python examples/py/mm_quote.py --config examples/config/mm_quote.toml --out runs/mm
.venv/bin/python examples/py/mm_quote.py --replay runs/mm         # 重算并比对全部输出
just build rel
build/rel/bin/pegged_mm --config examples/config/mm_quote.toml --out runs/mm-cpp   # C++ 版
build/rel/bin/pegged_mm --replay runs/mm-cpp
```

- `--out` 就是运行目录本身；不给时写到配置的 `persistence.dir`。
- 同一配置的 Python 与 C++ 版本写出的运行日志相同（配置 hash 也相同）。
- 回放用写出它的那个程序：Python 策略用策略文件的 `--replay`，C++ 策略用它自己的可执行文件；`jarvis replay` 只认识 `jarvis` 里注册过的 C++ 策略。
- 一整天的数据约 1400 万条输入。`dev` 构建带 ASan，跑完整一天要很久，所以 C++ 版用 `rel`；调试时用 `--set data.range.end=2024-03-30T00:10:00Z` 只跑前 10 分钟。Python 版的扩展模块总是 Release 构建。

回测指标：`examples/py/backtest_metrics.py` 对任一运行目录输出盈亏、成交、round trip、最大回撤与 Sharpe；`examples/py/trend_follow.py`（EMA 交叉的趋势策略，市价单）回测后直接打印：

```sh
.venv/bin/python examples/py/trend_follow.py --config examples/config/trend_follow.toml --out runs/trend
.venv/bin/python examples/py/backtest_metrics.py runs/mm [--interval 60] [--json metrics.json]
```

- 成交、手续费、盈亏与订单计数来自 `RunReport`（内核自己的 OMS 与账户算的）；权益曲线从运行日志重建，按最后成交价估值（与报表同口径），回撤、Sharpe 与 round trip 由它得出。最后一行 `check` 是曲线与报表净盈亏之差，应为 0。
- Sharpe 按 `--interval` 秒的权益收益年化，无风险利率为 0；数据只有几十个点时没有意义。只支持线性（USDT 保证金）合约与单币种账户。
- `trend_follow.py` 用自己的命令行（`--config`、`--env`、`--set`、`--out`、`--run-for`、`--interval`、`--json`），没有 `--replay`。

sandbox（生产行情加本地模拟交易所，需要能访问 Binance）：

```sh
.venv/bin/python examples/py/mm_quote.py --config examples/config/mm_quote.toml \
    --env sandbox --run-for 600 --out runs/mm-sandbox
```

- 所在地区访问不了 `fapi.binance.com`（HTTP 451）时，加 `--set venues.BINANCE_USDM.endpoint=testnet` 改用 testnet 的行情与 exchangeInfo，或用 `venues.BINANCE_USDM.exchange_info` 指向保存好的 exchangeInfo 响应。
- `Ctrl-C` 是优雅停止；之后同样可以 `--replay`。

live 节点（testnet 或生产）见 `docs/runbook.md`。

### 5.4 提交前

```sh
just format          # clang-format 改写全部 C++ 文件
just lint            # pre-commit 全部检查：格式、clang-tidy、CMake、分层、生成文件、actionlint
```

装了 `pre-commit install` 后，`git commit` 会对暂存的文件跑同样的检查；clang-tidy 依赖 `build/dev/compile_commands.json`，所以 `dev` 要配置过。

### 5.5 按改动选检查

`just check` 跑完整的一遍（lint、`just test`、golden、确定性门、基准 A/B、受影响的 TLA+ 规约），时间长，推送大改动前跑。平时按改动选：

| 改了什么 | 跑什么 |
| --- | --- |
| 任何 C++ | 相关的测试程序；`ctest --preset dev` |
| 内核（`jarvis/core`、`model`、`engine`、`execution`、`risk`、`backtest` 等） | `ctest --preset dev`；`just fp`（Release 与 `-O0` 的事件日志逐字节相同）；`just zero-alloc` |
| 输出或日志格式有意改变 | `just golden-update`，检查 `tests/golden/` 的 diff 后一起提交 |
| 规约（`specs/tla/`）或规约对应的代码（`tools/core-paths.txt` 列出） | `just tla-changed`；`just trace-forward <Spec>` |
| 多线程代码（`jarvis/live`、`jarvis/network`、环） | `just tsan`；`just chaos` |
| 解析、解码（WebSocket 帧、HTTP、JSON codec、配置、小数、日志格式） | `just fuzz <ws|http|json_codec|config|decimal|wire>` |
| 热路径性能 | `just bench-compare`（与 `main` 的 merge-base 做 A/B，默认 3 轮） |
| Python 层或绑定 | `.venv/bin/python -m pytest` |
| 文档、计划 | 无；功能完成时更新 `docs/plan.md` 对应的条目 |

其他命令：

```sh
just tsan                    # tsan preset 构建并跑全部 ctest
just chaos 20                # live 节点对故障交易所，20 个种子
just soak 600                # 同上一个种子跑 600 秒，再对日志做反向 trace 验证（需要 Java）
just fuzz ws 120             # 一个 fuzz 目标跑 120 秒；崩溃样本写在当前目录 crash-*
just bench                   # bench preset 的全部基准，结果在 benchmark-results/
just tla OrderLifecycle      # 一个规约的 TLC 检查；just tla 检查全部
just trace-backward runs/mm  # 运行日志投影到 OrderLifecycle 后交给 TLC；一整天约 2 万张订单，几分钟
```

`just` 的参数按位置传：`just fuzz ws 120`、`just bench-compare main 3`；写成 `target=ws` 会把这串文字当成参数值。

CI 的分层与这些命令对应：lint → functional（gcc-13、clang-18、macOS、Windows clang-cl、MinGW 交叉编译加 Wine）→ determinism、fuzz、bench-compare、formal；nightly 另跑 tsan、20 个种子的混沌测试、1 小时 soak 与更长的 fuzz（`.github/workflows/`）。

## 6. Windows

### 6.1 工具链

内核用 `__int128`，MSVC 的 cl.exe 没有这个类型，所以 Windows 上用 Clang 或 GCC：

- **clang-cl**（推荐）：Visual Studio 2022 自带的 Clang，用 MSVC 的 ABI 与标准库，编出的 Python 扩展能被 python.org 的 CPython 加载。`__int128` 的除法与乘法溢出检查要用 compiler-rt 的 builtins 库（`clang_rt.builtins-x86_64.lib`），CMake 在 configure 时向 clang-cl 查询它的路径并链接到每个目标。
- **MinGW-w64 GCC 13+**（MSYS2 或 Linux 上的交叉编译器）：能构建并运行 C++ 部分；不构建 Python 扩展，因为 MinGW 编出的扩展不能被 python.org 的 CPython 加载。
- 直接用 cl.exe 配置时 CMake 报错并指向本节。

### 6.2 安装

系统要求 Windows 10 1803 及以后或 Windows 11，x64。下面的命令在普通 PowerShell 里执行，用 winget 安装（`winget --version` 检查；没有时从 Microsoft Store 装"应用安装程序"）。包 ID 以 `winget search <名字>` 为准。

| 工具 | 必需 | 用途 |
| --- | --- | --- |
| Visual Studio 2022 Build Tools（或 Community 及以上），带 C++ 与 Clang 组件 | 是 | clang-cl、lld-link、MSVC 标准库、Windows SDK、CMake、Ninja |
| Git for Windows | 是 | 克隆仓库；它的 `sh` 运行 `justfile` 的命令 |
| OpenSSL 3 完整版（带头文件与导入库） | 是 | 网络层与 live shell；所有 preset 都打开 `JARVIS_BUILD_LIVE` |
| uv | 是 | 创建 `.venv`；系统上没有 Python 3.11 时自己下载一份 |
| just | 建议 | 命令入口（`justfile`） |
| LLVM 18 | 可选 | clang-format、clang-tidy、clangd 18，与 CI 的版本相同 |
| Java 17 | 可选 | TLA+ 模型检查 |
| MSYS2 | 可选 | MinGW 构建（第 6.5 节） |

#### 6.2.1 Visual Studio 2022

只要编译工具、不要 IDE 时装 Build Tools：

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --includeRecommended --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"
```

要 IDE 时装 Community（`--id Microsoft.VisualStudio.2022.Community`），把工作负载换成 `Microsoft.VisualStudio.Workload.NativeDesktop`，其余参数相同。已经装了 Visual Studio 的，打开 Visual Studio Installer，对已装的版本点"修改"，勾选：

- 工作负载"使用 C++ 的桌面开发"（推荐项里已含 MSVC v143 与 Windows SDK）；
- 单个组件 "C++ Clang Compiler for Windows"（`Microsoft.VisualStudio.Component.VC.Llvm.Clang`）：clang-cl、lld-link 与 compiler-rt；
- 单个组件 "MSBuild support for LLVM (clang-cl) toolset"（`...VC.Llvm.ClangToolset`）：Visual Studio 生成器的 `-T ClangCL` 用它；只用 Ninja 时可以不装；
- 单个组件 "C++ CMake tools for Windows"（`...VC.CMake.Project`）：CMake 与 Ninja。

版本：Visual Studio 2022 17.10 及以后自带的 CMake ≥ 3.25、clang-cl ≥ 17。更早的版本在 Installer 里"更新"，或另装 CMake（`winget install --id Kitware.CMake`）。

#### 6.2.2 其他工具

```powershell
winget install --id Git.Git
winget install --id astral-sh.uv
winget install --id Casey.Just
winget install --id ShiningLight.OpenSSL.Dev    # 完整版；ShiningLight.OpenSSL.Light 没有头文件，不能用
# 可选
winget install --id LLVM.LLVM --version 18.1.8
winget install --id EclipseAdoptium.Temurin.17.JDK
```

- Python 不必单独装：`just bootstrap` 运行 `uv venv --python 3.11`，系统上没有 3.11 时 uv 下载一份（MSVC 构建的 CPython，能加载 clang-cl 编出的扩展）。想用 python.org 的版本时 `winget install --id Python.Python.3.11`。
- OpenSSL 默认装在 `C:\Program Files\OpenSSL-Win64`，DLL 在其中的 `bin` 目录。winget 里找不到这个包时，从 slproweb.com 下载 "Win64 OpenSSL v3.x"（不是 Light 版）的安装包，或用 `choco install openssl`。
- just 也可以用 `uv tool install rust-just` 安装。

#### 6.2.3 环境变量

设置一次（用户级，之后新开的终端生效）：

```powershell
$ssl = "C:\Program Files\OpenSSL-Win64"     # OpenSSL 的安装目录
[Environment]::SetEnvironmentVariable("OPENSSL_ROOT_DIR", $ssl, "User")
$path = [Environment]::GetEnvironmentVariable("Path", "User")
[Environment]::SetEnvironmentVariable("Path", "$path;$ssl\bin;C:\Program Files\Git\bin", "User")
# 装了 LLVM 18 时：格式化与 lint 用它，不用 Visual Studio 自带的另一个版本
[Environment]::SetEnvironmentVariable("CLANG_FORMAT", "C:\Program Files\LLVM\bin\clang-format.exe", "User")
[Environment]::SetEnvironmentVariable("CLANG_TIDY", "C:\Program Files\LLVM\bin\clang-tidy.exe", "User")
```

- `OPENSSL_ROOT_DIR`：OpenSSL 装在 `C:\Program Files\OpenSSL` 或 `OpenSSL-Win64` 时 CMake 自己也能找到；设上更稳。
- `$ssl\bin`：测试程序和节点运行时加载 `libssl-3-x64.dll` 与 `libcrypto-3-x64.dll`。不在 `PATH` 上时程序启动即退出，没有任何输出。
- `C:\Program Files\Git\bin`：里面有 `sh.exe`，`justfile` 在 Windows 上用它执行命令。Git 安装程序默认只把 `Git\cmd` 加进 `PATH`，所以要自己加。不要加 `Git\usr\bin`：那里的 `link.exe` 等 GNU 工具与 MSVC 工具同名。

#### 6.2.4 进入 x64 开发者环境

clang-cl 要用 MSVC 标准库与 Windows SDK 的头文件和库，这些路径（`INCLUDE`、`LIB`、`PATH`）由 Visual Studio 的开发者环境设置。开始菜单里的 "Developer PowerShell for VS 2022" 默认的目标架构是 x86，链接 x64 程序会失败，所以用下面的函数进入 x64 环境。把它放进 PowerShell 配置文件（`New-Item -Force $PROFILE` 创建，`notepad $PROFILE` 编辑）：

```powershell
function Enter-VsDev64 {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -property installationPath   # -products *：也找 Build Tools
  Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
}
```

PowerShell 默认不运行配置文件（执行策略为 Restricted），先执行一次 `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`。以后每开一个终端先运行 `Enter-VsDev64`，再构建。也可以用开始菜单里的 "x64 Native Tools Command Prompt for VS 2022"（cmd）。

- Visual Studio 用"打开文件夹"打开仓库时，按 `win-*` preset 的 `architecture`、`toolset` 自己准备 x64 环境，不需要这一步。
- VS Code 在执行过 `Enter-VsDev64` 的终端里启动（`code .`），CMake Tools 与 clangd 就继承这个环境。

#### 6.2.5 检查

在执行过 `Enter-VsDev64` 的终端里：

```powershell
clang-cl --version                  # 17 或以后；Target: x86_64-pc-windows-msvc
cmake --version                     # 3.25 或以后
ninja --version
$env:VSCMD_ARG_TGT_ARCH             # x64
Test-Path "$env:OPENSSL_ROOT_DIR\include\openssl\ssl.h"    # True
where.exe libssl-3-x64.dll          # 能找到
sh --version                        # GNU bash（Git for Windows）
git --version; uv --version; just --version
```

另外两点：

- 仓库放在短路径下（如 `C:\src\jarvistrader`），依赖源码的路径不会超过 260 个字符。
- Windows Defender 的实时扫描会明显拖慢构建（大量小文件）。可以把仓库目录加入排除项（管理员 PowerShell：`Add-MpPreference -ExclusionPath C:\src\jarvistrader`）。

### 6.3 构建与测试

在 x64 开发者环境里：

```powershell
cd C:\src
git clone git@github.com:nivalume/jarvistrader.git; cd jarvistrader
just bootstrap                  # .venv（.venv\Scripts\python.exe）、scikit-build-core、pytest、pre-commit
just build                      # Windows 上 just 的默认 preset 是 win-dev（Debug，clang-cl）
ctest --preset win-dev
```

不用 just 时：

```powershell
cmake --preset win-dev "-DPython_EXECUTABLE=$PWD\.venv\Scripts\python.exe"
cmake --build --preset win-dev
ctest --preset win-dev
```

- 可执行文件是 `build\win-dev\bin\jarvis.exe` 等，测试程序在 `build\win-dev\tests\`；`tools/golden.py` 与 Python 测试会自己补 `.exe`。
- Release 构建用 `win-rel`：`just build win-rel`。
- 第一次 configure 要访问 GitHub 下载依赖，与 Linux 相同（第 3 节）。

VS Code：`launch.json` 的配置带 Windows 的路径（`"windows"` 字段）。`settings.json` 不能按系统区分，其中两处指向 `.venv/bin/python`，在 Windows 上用用户设置覆盖成 `.venv\Scripts\python.exe`：`cmake.configureSettings` 的 `Python_EXECUTABLE` 与 `python.defaultInterpreterPath`。`.clangd` 读 `build/dev` 的编译数据库，Windows 上在用户设置的 `clangd.arguments` 中加 `--compile-commands-dir=build/win-dev`。

### 6.4 Python 扩展

scikit-build-core 在 Windows 上默认用 Visual Studio 生成器与 cl.exe，所以安装前指定 Ninja 与 clang-cl（同样在 x64 开发者环境里）：

```powershell
$env:CMAKE_GENERATOR = "Ninja"; $env:CC = "clang-cl"; $env:CXX = "clang-cl"
just install                    # 或 just develop
.venv\Scripts\python.exe -m pytest
```

### 6.5 MinGW

在 Windows 上用 MSYS2：

```powershell
winget install --id MSYS2.MSYS2       # 装到 C:\msys64
```

从开始菜单打开 "MSYS2 MINGW64"（不是 "MSYS2 MSYS" 或 UCRT64）：

```sh
pacman -Syu                           # 第一次更新后窗口可能关闭；重新打开再执行一次
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-openssl mingw-w64-x86_64-python
cd /c/src/jarvistrader
cmake --preset mingw-dev && cmake --build --preset mingw-dev && ctest --preset mingw-dev
```

`mingw-dev` 不构建 Python 扩展；可执行文件依赖 MinGW 的 DLL，在 MINGW64 shell 之外运行要把 `C:\msys64\mingw64\bin` 加进 `PATH`。

在 Linux 上交叉编译并用 Wine 跑测试（CI 的 `mingw-cross` job 也是这样做的）：

```sh
sudo apt-get install g++-mingw-w64-x86-64-posix wine64 zstd
# OpenSSL：MSYS2 mingw64 仓库的包，解开后用环境变量指过去
pkg=mingw-w64-x86_64-openssl-3.6.5-1-any.pkg.tar.zst
mkdir -p build/mingw-prefix && curl -fsSL -o build/mingw-prefix/$pkg https://repo.msys2.org/mingw/mingw64/$pkg
tar --zstd -xf build/mingw-prefix/$pkg -C build/mingw-prefix
export JARVIS_MINGW_PREFIX=$PWD/build/mingw-prefix/mingw64 WINEDEBUG=-all
cmake --preset mingw-cross && cmake --build --preset mingw-cross -j4
ctest --preset mingw-cross      # 每个测试程序经 wine64 运行
```

工具链文件是 `cmake/toolchains/mingw-w64.cmake`：用 `x86_64-w64-mingw32-g++-posix`（`std::thread` 要 posix 线程模型），静态链接 libstdc++ 与 OpenSSL，所以交叉编译出的 `.exe` 不依赖 MinGW 的 DLL。

### 6.6 与 Linux、macOS 的差别

- **确定性**：Windows 构建写出的事件日志与 Linux 的逐字节相同（`jarvis corpus` 的指纹与全部 golden 用例都验证过）；日志头记录的编译器与平台字符串不同，不计入指纹。
- **行尾**：`.gitattributes` 让所有文本文件在 Windows 上也以 LF 检出，golden 期望文件、配置与测试输入逐字节比较时才一致。程序写的文件都以二进制模式打开，不会写出 CRLF。
- **TOML 里的路径**：双引号字符串中的 `\` 是转义符，写 `"C:\data"` 会解析失败。用单引号的字面字符串 `'C:\data\catalog'` 或正斜杠 `"C:/data/catalog"`。命令行 `--set data.catalog=C:\data\catalog` 不受影响。
- **admin socket**：Unix 域 socket 需要 Windows 10 1803 及以后，路径如 `unix://C:/jarvis/run/mm01.sock`（最长 107 字节）；更早的系统上 admin 线程启动失败并说明原因。socket 的访问由所在目录的 ACL 决定。
- **密钥文件**：没有 0600 这样的权限位，节点不检查，用 ACL 限制（`docs/runbook.md` 第 3.1 节）。
- **停止**：Ctrl+C、Ctrl+Break 等同 SIGINT；关闭控制台窗口、注销或关机时系统最多给约 5 秒。`ensure_hash_seed` 在 Windows 上不能 exec，改为启动子进程并等待它结束，退出码相同。
- **绑核**：`[threads]` 可用，CPU 编号限于进程所在 processor group 的 0–63。
- **持久化**：`FlushFileBuffers` 代替 `fdatasync`，通常更慢；`barrier` 模式的命令延迟相应更高。
- **只在 Linux、macOS 上有的**：`dev` 的 ASan/UBSan、`tsan`、`fuzz`、`bench` 与 `det-o0` preset，`just fp`、`just tla`、`just bench-compare` 等脚本类命令。生产实盘只支持 Linux（`docs/architecture.md` 第 19.6 节）。

## 7. 常见问题

- **configure 报找不到 OpenSSL**：Linux 装 `libssl-dev`；macOS 设 `OPENSSL_ROOT_DIR`（第 2.2 节）。
- **configure 时下载依赖失败**：检查到 GitHub 的网络与代理；设 `CPM_SOURCE_CACHE` 后，已下载的依赖不再重复下载。
- **编译被系统杀掉（OOM）**：降低并行数，`CMAKE_BUILD_PARALLEL_LEVEL=4`。
- **clangd 报找不到头文件或满屏错误**：先 `just configure`，确认 `build/dev/compile_commands.json` 存在；clangd 改用新的数据库后重启语言服务。
- **`just lint` 说 compile_commands.json 不存在**：同上，`just configure dev`。
- **格式检查在 CI 失败、本地通过**：本地的 clang-format 不是 18；设 `CLANG_FORMAT`（第 2 节）后 `just format`。
- **编辑器说 `jarvis.model` 等没有某个属性**：这些名字来自编译出的 `jarvis._core`，仓库还没有为它生成类型存根（`.pyi`），pyright 与 Pylance 看不到它们的成员；不影响运行。
- **`import jarvis` 用的不是当前代码**：`just test` 或 `just install` 换掉了可编辑安装；`just develop` 再装一次。用 `python -c "import jarvis; print(jarvis.__file__, jarvis.build_info())"` 查看。
- **`jarvis.Node` 不能跑 sandbox**：安装的是不带 live shell 的包（`build_info()` 的 `live_enabled` 为 False）；用 `just develop` 或 `just install-live`。
- **在调试器里运行 ASan 程序报 LeakSanitizer 错误**：设 `ASAN_OPTIONS=detect_leaks=0`。
- **调试 `mm_quote.py` 以 `SystemExit: 1` 结束**：看调试终端里 `mm_quote.py:` 开头的错误行。`no data for ... NotFound`：先下载数据（5.3 节）；`already holds a run log: AlreadyExists`：`--out` 目录里已有一次运行，删掉或换目录（共享的 `launch.json` 在启动前已清理 `runs/mm-dbg`）。
- **macOS 上 `[threads]` 的节点启动失败**：绑核只支持 Linux 与 Windows，删掉 `[threads]` 中的 CPU 设置。
- **Windows 上 configure 报 cl.exe 不受支持**：在 x64 开发者环境里用 `win-dev`／`win-rel` preset（指定 clang-cl），或给 Visual Studio 生成器加 `-T ClangCL`（第 6.1 节）。
- **Windows 上 configure 找不到 clang-cl、`stddef.h`，或链接报 `machine type x86 conflicts with x64`、找不到 `kernel32.lib`**：终端不在 x64 开发者环境里，先运行 `Enter-VsDev64`（第 6.2.4 节）；用 `$env:VSCMD_ARG_TGT_ARCH` 确认是 `x64`。
- **Windows 上 `just` 报找不到 `sh`**：`C:\Program Files\Git\bin` 不在 `PATH` 上（第 6.2.3 节）。
- **Windows 上 configure 报找不到 clang_rt.builtins**：Visual Studio Installer 里装单个组件 "C++ Clang Compiler for Windows"（第 6.2.1 节）；单独装的 LLVM 也带这个库。
- **Windows 上测试程序启动即退出、没有输出**：找不到 OpenSSL 的 DLL，把 OpenSSL 的 `bin` 目录加进 `PATH`。
- **Windows 上 TOML 配置报 `Error while parsing unicode scalar sequence`**：双引号字符串里的路径含 `\u`、`\U` 之类被当成转义，改用单引号或正斜杠（第 6.6 节）。
- **回放出现偏差（退出码 3）**：见 `docs/runbook.md` 第 8.10 节。
