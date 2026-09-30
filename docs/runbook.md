# 运维手册

本手册面向运行 sandbox 与 live 节点的人：部署、配置、密钥、监控告警与故障处理。设计上的理由见 `docs/architecture.md`，文中的节号都指它。

- 生产实盘只支持 Linux。macOS 可以做开发、回测与 testnet。
- 当前只有 Binance USDⓈ-M 永续合约（`kind = "binance_usdm"`），单向持仓（`account_mode = "one_way"`，默认）；hedge 模式的 OMS 属于 M6。
- 一个节点进程就是一个 Node：一个 core 线程，行情、venue-io、persist、telemetry、admin 各一个线程（§7.1）。

## 1. 部署

### 1.1 构建

两种节点，命令行相同（§4.5）：

- C++ 节点：策略编译进可执行文件，入口 `jarvis::live_node_main<S>`（`jarvis/live/live_main.hpp`）。示例 `examples/cpp/pegged_mm.cpp`。
  ```
  cmake --preset rel && cmake --build build/rel -j4
  build/rel/bin/pegged_mm --config node.toml
  ```
  `rel` preset 打开了 `JARVIS_BUILD_LIVE`，需要 OpenSSL 3 的头文件。
- Python 节点：策略文件末尾调用 `jarvis.main(MyStrategy)`。安装带 live shell 的包：
  ```
  just install-live
  python my_strategy.py --config node.toml
  ```
  C++ 策略插件用 `jarvis.load_native(path)` 加载（§7.3，M5-N）。插件的编译器与共享类型的布局必须与节点一致（入口报告布局指纹），否则加载时拒绝。

命令行：

```
<node> --config FILE [--env ENV] [--set path=value]... [--out DIR] [--run-for SECONDS]
<node> --replay RUN_DIR [--from-snapshot FILE] [--until SEQ] [--dump-state]
```

- `--env` 覆盖 `node.env`，`--set` 覆盖任意键（`strategies.<id>.params.<key>=...` 按 id 选策略）。覆盖写进运行目录的 `run.toml`，回放时照样应用。
- 退出码：0 正常；1 失败（配置、启动检查、运行中出错，或策略错误按 `halt_node` 停掉了节点）；2 命令行错误；3 回放出现偏差。

### 1.2 主机准备

- 时钟：chrony 同步。启动检查要求本机时钟与交易所相差不超过 1000 ms，否则拒绝启动。
- CPU（§19.6）：
  - 给 core 线程留一个 `isolcpus` 隔离的核，用 `[threads] core_cpu` 绑定；
  - IO 线程与 core 在同一 NUMA 节点（`numa_node`）；
  - 网卡中断绑到非 core 的核；
  - 关闭透明大页的自动合并（`echo madvise > /sys/kernel/mm/transparent_hugepage/enabled`）。
- 用户与目录：
  - 以专用用户运行；
  - `persistence.dir` 所在磁盘要有余量（见第 5 节）；
  - admin socket 的目录（如 `/run/jarvis`）归该用户所有。
- systemd：`SIGTERM` 触发优雅关停（第 4.3 节）。`TimeoutStopSec` 要大于 `node.shutdown_timeout_ms` 加 5 秒，否则 systemd 会在撤单确认前 `SIGKILL`。

```ini
[Service]
User=jarvis
ExecStart=/opt/jarvis/bin/pegged_mm --config /etc/jarvis/mm01.toml
KillSignal=SIGTERM
TimeoutStopSec=30
Restart=no
LimitNOFILE=65536
```

不要设 `Restart=always`。节点异常退出后，先按第 8 节判断原因、确认交易所上的挂单，再手工启动。

### 1.3 上线顺序

1. backtest：`node.env = "backtest"`，用历史数据确认策略行为。
2. sandbox：`node.env = "sandbox"`，生产行情加本地模拟交易所，至少运行 24 小时；用 `--replay` 确认录制可以逐字节回放。
3. testnet：`node.env = "live"`，`venues[0].endpoint = "testnet"`，testnet 的 API key。testnet 没有 spot 主机，key 权限检查会跳过并给出警告。
4. 生产小资金：`endpoint = "prod"`，风控限额设小（`[risk]`），`persistence.mode = "barrier"`。

## 2. 配置

完整示例见 `examples/config/node.toml` 与 §4.2。`jarvis config <file>` 打印两部分规范形式与配置 hash。

### 2.1 hash 覆盖的部分与运维参数

- 进 hash：`[node]` 的 id、seed、capacity、shutdown、`market_data_stale_ms` 等，venue 的 kind、account_mode、oms、leverage、sim，`[[strategies]]`，`[risk]`，`[python]` 的回调预算。这些决定内核的计算结果；改了它们，`persistence.resume` 不能接着上一次运行（第 8.9 节）。
- 不进 hash：`node.env`、`[data]`、venue 的 endpoint、credentials、exchange_info，`[persistence]`、`[telemetry]`、`[admin]`、`[threads]`、`python.idle_hook_ms`。

### 2.2 live 节点的检查单

| 键 | 建议 | 说明 |
| --- | --- | --- |
| `node.id` | 1–8 个字母或数字，每个节点唯一 | 进入 `ClientOrderId`，对账据此认出本节点的订单 |
| `node.shutdown` | `cancel_all_then_exit` | 停机时撤单（第 4.3 节） |
| `node.shutdown_timeout_ms` | 10000 | 等撤单确认的上限 |
| `node.market_data_stale_ms` | 10000 | 行情静默这么久算陈旧，节点降为 `Degraded` |
| `venues[0].endpoint` | `prod` 或 `testnet` | 没有默认指向生产的开关 |
| `venues[0].credentials` | `env:` 或 `file:` 引用 | 第 3 节 |
| `venues[0].leverage` | 与交易所账户一致 | 启动检查核对每个合约的杠杆；0 表示不查 |
| `risk.countdown_cancel_all_ms` | 120000（至少 10000） | 死人开关：节点失联这么久后交易所撤掉全部挂单 |
| `risk.max_order_notional`、`max_position_notional`、`daily_loss_limit`、`daily_loss_halt` | 按资金设定 | 超限降为 `Reducing` 或 `Halted` |
| `risk.orders_per_10s`、`orders_per_minute` | 低于交易所限额 | 本地令牌桶 |
| `risk.on_strategy_error` | `halt_strategy` | 策略抛异常时停用该策略；`halt_node` 停整个节点 |
| `persistence.mode` | `barrier` | 命令的记录落盘后才发出，交易所见过的命令一定在本地日志中（§16.2） |
| `persistence.resume` | `true` | 崩溃或重启后接着上一次运行（第 8.9 节） |
| `persistence.truncate` | 视磁盘而定 | 完整快照后删除旧日志段 |
| `persistence.raw_frames` | `true` | 原始帧用于排查解码问题，占盘最多 |
| `telemetry.prometheus` | `127.0.0.1:9100` | 只在本机或内网暴露 |
| `admin.socket` | `unix:///run/jarvis/{node_id}.sock` | 第 7 节 |
| `[threads]` | 生产机上设置 | §19.6；不设置则不绑核 |

sandbox 用 `[venues.sim]` 设定模拟交易所（余额、费率、成交模型）。live 节点有 `[venues.sim]` 时拒绝启动。

## 3. 密钥

### 3.1 引用方式

配置中只写引用，不写 secret（§19.1）：

- `env:PREFIX`：读取 `PREFIX_API_KEY`，以及 `PREFIX_API_SECRET`（HMAC）或 `PREFIX_PRIVATE_KEY_FILE`（Ed25519 私钥文件，PEM）。
- `file:/etc/jarvis/keys/mm01.toml`：TOML 文件，含 `api_key`，以及 `secret` 或 `private_key_file`（相对该文件的路径）。

保存 secret 的文件（TOML 文件与私钥文件）权限必须是 0600 或 0400，属主是运行节点的用户。组或其他用户可读时节点拒绝启动，报错给出当前权限和 `chmod 600` 命令。错误信息从不包含 secret。

建议用 Ed25519 key：签名用私钥，交易所只保存公钥。

### 3.2 key 的要求（启动检查）

- 可以交易合约；
- 不能提现，否则拒绝启动；
- 必须设置 IP 白名单，否则拒绝启动（命令行没有绕过的开关）。

### 3.3 轮换

1. 在交易所新建 key（同样的权限与白名单）。
2. 更新引用指向的环境变量或文件。credentials 不进配置 hash，`persistence.resume` 不受影响。
3. 优雅停机（`SIGTERM` 或 `jarvis admin ... shutdown`），确认输出中没有 `left open at stop`。
4. 启动节点，确认启动检查通过、`/ready` 为 200。
5. 在交易所删除旧 key。

## 4. 启动与停止

### 4.1 启动检查

`env = "live"` 时，节点在连接行情之前依次检查（§14.6，`jarvis/adapter/binance/startup.hpp`），任一项失败即退出，错误列出全部失败项：

| 检查 | 失败信息示例 | 处理 |
| --- | --- | --- |
| exchangeInfo 中有配置的合约 | `exchangeInfo: ...` | 核对 `[data.streams]` 的 instrument；生产 REST 不可达时可用 `venues[0].exchange_info` 指向保存的响应 |
| 持仓模式 | `the account is in hedge ...` | 在交易所把账户改为单向持仓 |
| 联合保证金 | `multi-assets mode is on ...` | 关闭联合保证金 |
| 保证金类型 | `BTCUSDT uses isolated margin; the node expects cross` | 在交易所改为全仓 |
| 杠杆 | `BTCUSDT leverage is 20; the node expects 5` | 改交易所杠杆或 `venues[0].leverage` |
| key 权限 | `the API key may withdraw; a trading key must not` | 第 3.2 节 |
| 时钟 | `the local clock is 1500 ms off the venue's` | 检查 chrony |

之后节点取下一个 ClientOrderId epoch（写入 `persistence.dir` 旁的 `epoch` 文件，先落盘再下单），启动各线程，进入 `Syncing`。

### 4.2 运行中的状态

Node 状态（`jarvis_node_state`，§4.4）：

- `Syncing`：等待对账完成、下单通道 up。策略收不到行情，不能下单。
- `Running`：正常交易，`/ready` 为 200。
- `Degraded`：用户流、行情或下单通道断开，或行情陈旧。停止交易，恢复后重新对账回到 `Running`。
- `Stopping`：停机中，等撤单确认。
- `Stopped`、`Faulted`：已结束。

TradingState（`jarvis_trading_state`，§10.2）：`Active` 正常；`Reducing` 只允许减仓；`Halted` 不允许下单。风控超限、对账差异、admin 命令都会改变它。

### 4.3 停止

- `SIGTERM`、`SIGINT`、`jarvis admin <socket> shutdown` 与 `--run-for` 到期效果相同：步进一个记录的 `Shutdown` 输入。
- `cancel_all_then_exit`（默认）：
  1. TradingState 置 `Halted`，撤销全部挂单；
  2. 等撤单确认，最多 `shutdown_timeout_ms`；
  3. 全部确认后以 `countdownTime = 0` 解除死人开关；仍有挂单时不解除，交易所在倒计时结束时撤单；
  4. 写最终快照，下一次 `persistence.resume` 从它继续，不需要重算日志。
- `exit_keep_orders`：不撤单，订单留在交易所。
- 结束时打印运行摘要。出现 `left open at stop: N orders` 说明有 N 张订单没等到确认，去交易所核对（第 8.8 节）。

## 5. 运行目录与数据

每次运行一个目录，默认 `runs/{node_id}/{run_id}`（§16.1）：

| 文件 | 内容 |
| --- | --- |
| `config.toml` | 节点加载的配置原文 |
| `run.toml` | `--env`、`--set` 覆盖，配置 hash，续跑时的来源 `resumed_from` |
| `events-*.jlog` | 运行日志：每条输入及其输出，回放与恢复都靠它 |
| `snapshot-*.jsnap` | 引擎状态快照，每 `snapshot_every` 条输入一个 |
| `raw-frames.jraw`、`raw-account.jraw` | 行情与账户连接的原始帧（`persistence.raw_frames`） |
| `telemetry.jsonl` | 遥测记录，每行一个 JSON，订单相关的行带 `client_order_id` |

- `epoch` 文件在 `persistence.dir` 中 `{node_id}` 这一层，记录已用过的最大 epoch。不要删除或回退它，否则新订单的 ClientOrderId 可能与旧订单重复。
- 磁盘：原始帧最大，BTCUSDT 的 `aggTrade`、`bookTicker`、`depth@100ms` 两小时约 780 MB（M4 验收）。长期运行时打开 `persistence.truncate`，并定期把旧运行目录移到别处。
- 备份：运行目录可以直接复制。`--replay` 只需要目录本身和同一构建的节点程序。

## 6. 监控与告警

### 6.1 端点

`[telemetry] prometheus` 设置后：

- `/metrics`：Prometheus 文本；
- `/ready`：`Running` 时 200，否则 503；
- `/live`：core 线程 5 秒内发布过样本时 200。

`jarvis admin <socket> status` 返回同样的 ready 与 alive，以及状态与最后的 `seq`。指标清单见 §19.2。

### 6.2 建议的告警

| 条件 | 含义 | 处理 |
| --- | --- | --- |
| `/live` 非 200 超过 10 s | core 线程卡住或进程已退出 | 第 8.7 节 |
| `jarvis_ready == 0` 超过 60 s | 未同步、降级或行情陈旧 | 看 `jarvis_node_state` 与 `jarvis_connection_up`，第 8.1–8.3 节 |
| `jarvis_node_state{state="FAULTED"} == 1` | 节点出错退出 | 第 8.7 节 |
| `jarvis_trading_state{state="HALTED"} == 1` 或 `REDUCING` | 风控、对账差异或人工停机 | 查 `telemetry.jsonl` 中 `"event":"trading_state"` 的行（`from`、`to`）与它前面的输入 |
| `increase(jarvis_reconcile_diffs_total[5m]) > 0` 或 `jarvis_light_check_diffs_total` 增加 | 本地与交易所不一致 | 第 8.4 节 |
| `increase(jarvis_connection_downs_total[10m]) > 3` | 连接反复断开 | 查网络与交易所公告 |
| `jarvis_market_data_age_ns > 5e9` | 行情静默 | 第 8.1 节 |
| `increase(jarvis_http_429_total[5m]) > 0` | REST 限速 | 第 8.5 节 |
| `increase(jarvis_http_418_total[5m]) > 0` | IP 被封 | 第 8.5 节 |
| `jarvis_rate_limit_remaining` 接近 0 | 本地令牌桶将尽 | 降低下单频率或调低策略节奏 |
| `increase(jarvis_countdown_failures_total[5m]) > 0` | 死人开关续期失败 | 交易所上的倒计时可能到期撤单，查 REST 连通性 |
| `increase(jarvis_listen_key_failures_total[10m]) > 0` | listenKey 创建或续期失败 | 查 REST 连通性，用户流会断开并重连 |
| `increase(jarvis_venue_unknown_outcomes_total[5m]) > 0` | 下单结果未知（超时、5xx、在途断线） | 由对账确认；持续出现时查网络 |
| `increase(jarvis_feed_decode_errors_total[5m]) > 0` 或 `jarvis_venue_decode_errors_total` | 交易所消息格式变化 | 保留原始帧，升级适配器 |
| `jarvis_log_durable_lag_bytes` 持续增长或 `jarvis_log_ring_stalls_total` 增加 | 磁盘跟不上 | 第 8.6 节 |
| `increase(jarvis_telemetry_dropped_total[5m]) > 0` | 遥测环满，记录丢失 | 只影响遥测；持续出现时查磁盘 |
| `jarvis_ring_high_water / jarvis_ring_capacity > 0.5` | core 处理不过来 | 查策略回调耗时 `jarvis_strategy_callback_max_ns`、`jarvis_step_ns` |
| `increase(jarvis_strategy_overruns_total[5m]) > 0` | Python 回调超出 `python.callback_budget_us` | 优化策略；连续 `python.overrun_limit` 次超限产生策略错误，按 `risk.on_strategy_error` 处理 |
| `jarvis_command_to_socket_ns` 的 p99 > 1 ms（不 busy-poll）或 > 500 µs（busy-poll） | 发单延迟超出目标（§17.3） | 查 CPU 争用、绑核 |

## 7. admin 命令

`[admin] socket` 设置后可用；socket 权限 0600，只有属主能发命令（§19.3）。

```
jarvis admin /run/jarvis/mm01.sock status
jarvis admin --config /etc/jarvis/mm01.toml halt
```

| 命令 | 作用 | 记录进日志 |
| --- | --- | --- |
| `status` | 一行 JSON：Node 状态、TradingState、`seq`、ready、alive | 否 |
| `halt` | TradingState 置 `Halted`，不再下单，挂单保留 | 是 |
| `reduce` | 置 `Reducing`，只允许减仓 | 是 |
| `resume` | 回到 `Active`，清除监控与轻量对账造成的降级 | 是 |
| `cancel_all` | 撤销全部挂单，不改变 TradingState | 是 |
| `set_param <strategy_id> <key> <value>` | 交给策略的 `on_params_changed` | 是 |
| `snapshot` | 下一个批次边界取快照 | 是 |
| `shutdown` | 按 `node.shutdown` 停机，与 `SIGTERM` 相同 | 是 |

除 `status` 外的命令都是记录的输入，回放时复现，事后可以审计。回复以 `error` 开头时命令行退出码为 1。

## 8. 故障处理

先看三样东西：`jarvis admin <socket> status`、`/metrics` 中的 `jarvis_node_state` 与 `jarvis_trading_state`、运行目录的 `telemetry.jsonl` 末尾。

### 8.1 行情断开或陈旧

- 现象：`jarvis_connection_up{connection="MARKET_DATA"} == 0`，或 `jarvis_market_data_age_ns` 超过 `market_data_stale_ms`；节点 `Degraded`，`/ready` 为 503。
- 节点的动作：停止交易；行情线程按退避重连（500 ms 起，最长 30 s），深度簿重新取快照同步；行情恢复后回到 `Running`。
- 人工：一般不需要。长时间不恢复时查网络与交易所状态。需要撤单时用 `cancel_all`。

### 8.2 用户流断开

- 现象：`jarvis_connection_up{connection="USER_STREAM"} == 0`；节点 `Degraded`。
- 节点的动作：暂存交易所事件，重连用户流，重连后取一次对账快照（REST），对账完成后回到 `Running`。listenKey 过期时 REST 线程重建。
- 人工：`jarvis_venue_snapshot_failures_total` 持续增加说明 REST 不通，查网络与限速。

### 8.3 下单通道（WS API）断开

- 现象：`jarvis_connection_up{connection="ORDER_ENTRY"} == 0`。
- 节点的动作：命令改走 REST（`jarvis_rest_orders_total` 增加）；关闭 REST 兜底时命令在本地被拒，原因 `BINANCE_0 order entry is down`。结果未知的订单留给对账。

### 8.4 对账差异

- 现象：`jarvis_reconcile_diffs_total{kind}` 或 `jarvis_light_check_diffs_total` 增加；`telemetry.jsonl` 中有 `ReconciliationDiff`。
- 差异种类：
  - `POSITION`、`BALANCE`：交易所的仓位或余额与本地不同，对账以交易所为准置位；
  - `FILLED_QUANTITY`：有成交未到达，对账用 `userTrades` 补出合成成交；
  - `LOST_ORDER`：本地的挂单交易所不认识，按丢失关闭；
  - `EXTERNAL_ORDER`：交易所上有节点没下的单。带本节点标签的（如上一次运行没有 resume 留下的）会被撤销；其他来源的只报告；
  - `UNTRACKED_ORDER`（轻量对账）：本地认为已结束的订单在交易所仍挂着。
- 轻量对账的差异会把 TradingState 降为 `Reducing`，不改本地状态。
- 人工：
  1. 在交易所核对该合约的仓位与挂单；
  2. 有人手工下单或另一个进程用同一账户时，先停掉它；
  3. 确认一致后 `resume`。差异反复出现时停机，保留运行目录。

### 8.5 限速与封禁

- 429：REST 请求过多。节点在 `Retry-After`（缺省 10 秒）内不再发出 REST 请求（§14.7）；这期间走 REST 的下单在本地被拒（原因以 `BINANCE_0` 开头），快照稍后重试。持续出现时降低策略的下单频率，检查同一 IP 上是否有其他程序。
- 418：IP 被封。封禁期间（`Retry-After`，缺省 120 秒）REST 请求在本地直接失败，不再发出。等封禁结束；期间节点可能无法对账而停在 `Degraded`。需要立即撤单时从交易所网页操作，或等死人开关到期。

### 8.6 持久化失败或磁盘满

- 现象：`jarvis_log_ring_stalls_total` 增加（磁盘慢）；写入或 `fdatasync` 失败时节点 `Faulted` 退出。
- `barrier` 模式下，没落盘的命令不会发出。
- 人工：清理磁盘，按第 8.7 节重启。

### 8.7 节点退出（Faulted、崩溃、被杀）

1. 看退出信息：`the node faulted at seq N: <status>`，或 systemd 日志中的信号。
2. 交易所上的挂单：
   - `Faulted` 时 driver 已尽力撤单（KillSwitch）；
   - 崩溃或 `SIGKILL` 时挂单留在交易所，死人开关在 `countdown_cancel_all_ms` 后撤掉它们。
   - 需要立即撤单时从交易所网页或 API 操作。
3. 保留运行目录，用 `--replay RUN_DIR` 回放。回放无偏差说明内核按日志正常运行，问题在外部（网络、磁盘、交易所）。有偏差见第 8.10 节。
4. 重启：`persistence.resume = true` 时新运行从上次的快照与日志恢复（第 8.9 节），重新对账后继续。

### 8.8 停机时仍有挂单

- 现象：摘要中 `left open at stop: N orders`。
- 原因：`shutdown_timeout_ms` 内没收到撤单确认（用户流断开、交易所慢）。
- 死人开关没有解除，交易所会在倒计时结束时撤单。立即在交易所核对；需要时手工撤单。

### 8.9 恢复（resume）被拒绝

`persistence.resume = true` 时以下情况拒绝启动，信息以 `cannot resume` 开头：

| 信息 | 原因 | 处理 |
| --- | --- | --- |
| `it ran a different configuration` | 配置 hash 变了 | 改回原配置；或确认交易所无挂单后以 `--set persistence.resume=false` 重新开始 |
| `a strategy does not describe its own state` | 策略没有实现 `state(ar)`（Python：`on_save`、`on_load`） | 实现它，或不用 resume |
| `its log does not replay at seq N` | 日志回放出现偏差 | 第 8.10 节 |
| 快照由别的构建写出 | 升级了程序 | 第 9 节 |

`resume = false` 重新开始时，对账以交易所为准重建仓位；本节点上一次留下的挂单作为 `EXTERNAL_ORDER` 被撤销。

### 8.10 回放偏差

- `--replay` 退出码 3，并给出第一个不一致的 `seq`、记录的输出与重算的输出。
- 排查：
  1. `--until SEQ --dump-state` 输出该点的内核状态；
  2. 确认用的是同一构建（日志头记录提交号）与同一策略代码；
  3. Python 策略须用策略文件自身的 `--replay`；
  4. 策略里读了墙钟、随机数或外部状态是最常见的原因（§4.6）。
- 偏差在内核或适配器中时，保留运行目录并报告。

## 9. 升级

快照属于构建：状态布局随代码变化，别的构建写出的快照不用于恢复。升级程序时：

1. 优雅停机，确认没有 `left open at stop`。
2. 部署新构建。
3. 以 `--set persistence.resume=false` 启动（或在配置中改为 false），对账从交易所重建状态。
4. 确认 `/ready` 为 200、没有对账差异，再把 `resume` 改回 true。

只改运维参数（endpoint、credentials、`[persistence]`、`[telemetry]`、`[admin]`、`[threads]`）不影响配置 hash，可以带 `resume` 直接重启。

## 10. 尚未覆盖的部分

以下验收需要 API key 或真实交易日志，本仓库的 CI 没有：

- testnet 连续 72 小时运行，穿越强制断线与 listenKey 过期；
- 在 testnet 上执行混沌测试（本地模拟交易所上的混沌测试见 `tests/cpp/test_chaos.cpp`）；
- 用实盘日志做 `Reconciliation` 与 `NodeLifecycle` 的反向 trace validation；
- testnet 契约测试（每类流与每个 WS API 方法的往返）。
