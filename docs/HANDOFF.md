# 交接文档：沙箱内部重构

写给下一个接手的会话。读完这份就该知道：改到哪了、怎么验证、还剩什么、哪些坑已经踩过。

> **接手上一个会话的直接任务**（单次执行/批量任务的 workspace 配额，决策 A）请看
> `docs/HANDOFF_QUOTA.md`：那里面有现状、7 步实现清单、验收标准、这次踩过的坑和这套环境的
> 工作流。本文档讲整体设计范围，不重复这些。

## 1. 现在的状态

| 项 | 值 |
|---|---|
| 仓库 | `D:\cpp_project\linux-sandbox`（工作目录唯一权威） |
| 提交 | `4d1c802` 之后又落了会话配额与单次配额两组改动；工作副本在 `ed4758b` 之上实现决策 A（见 `HANDOFF_QUOTA.md`） |
| 测试 | **33/33 通过**（`ctest --test-dir build`；`isolation_integration` 无委派时 skip，skip 不算通过） |
| 隔离验收 | **本轮修复后全绿**（`bash tools/isolated-accept.sh`）：必检行全齐（含新增的 `single-run quota enforced and the partial tree was exported`、`a refused export is reported, not passed off as success`），`NOT EXECUTED` 为 0，服务 `code=exited/status=0`，sysctl 按原值恢复 |
| 跳过项 | 不放开 `kernel.apparmor_restrict_unprivileged_userns` 时 `isolation_integration` 会 skip，这是预期行为，**但 skip 等于没测** |

### 1.1 本轮（`ec5e441` 之后）的 7 个提交

| 提交 | 内容 |
|---|---|
| `7c608e8` | 会话命令按类型化记录落盘（v3 记录格式，v2 只读兼容） |
| `c4aa6fd` | `/input` 的 `snapshot_copy`：launcher 之前复制，默认仍是只读绑定 |
| `acc43b2` | Protobuf 直接解成业务请求，控制器只收一个处理函数 |
| `fa2c51c` | 复制循环改用 `admit()`，拒绝原因进错误文本 |
| `eeef4ea` | 会话每条命令一个 `ExecutionCore`；`CommandRecord`/`CommandFacts` 退出运行时 |
| `6cb8bd5` | 删除遗留 `CommandRecord`/`CommandFacts`/`StopReason`/`CommandPhase` |
| `4d1c802` | `SessionStore` 返回类型化 `CommandReport`，回包渲染搬到 wire 层 |
| 本轮之后 | `src/runtime/init.cpp` 合并了两份 PID 1（P5），`persistent_init.cpp` 删除 |

工作副本在 `C:\Users\lipinzheng\documents\deepseek-harness\default-workspace\sbx`（同一个 git 仓库的 clone，分支 `master`）。

**同步方式**：改在 clone 里 → `git commit` → `git -C D:\... fetch <clone> master && git merge --ff-only FETCH_HEAD`。不要直接改 D 盘，也不要整目录覆盖。

## 2. 怎么验证

### 主机侧（只能跑通纯逻辑）

```powershell
powershell -ExecutionPolicy Bypass -File tools\portable-tests.ps1
powershell -ExecutionPolicy Bypass -File tools\cross-verify.ps1 -Zig <zig.exe> -HostWork <work>
```

`cross-verify.ps1`（需要 `-ZigHostWork <目录>`）做 8 段检查：9 套可移植测试 → profiles → CMake 源列表一致性 → 全部源 TU 编译 → 全部测试 TU 编译 → 2 个二进制链接 → 每个测试可执行文件各链接一次。**这里不写死数量**：加了测试就会变，写死只会让文档过期。**它不跑 CMake 的库边界链接**，所以像 "`sandbox_model` 没链接 `sandbox_core`" 这类错误它查不出来——那是第一次上真机才暴露的。

### 虚拟机侧（唯一有意义的验证）

虚拟机是 VMware 上的 Ubuntu，IP `192.168.244.128`。上面跑着一个受控服务：

```
guest: ~/sbxctl/srv.py    监听 0.0.0.0:8000
  POST /exec   body=shell 脚本 → 返回 stdout+stderr
  POST /put/<path>  写文件到 ~/sbxctl/<path>
  GET  /get/<path>  读文件
```

主机侧用 `Invoke-WebRequest` 调用即可，不需要人参与：

```powershell
$ip='192.168.244.128'
function Invoke-Guest([string]$script, [int]$timeout=400){
  (Invoke-WebRequest -Uri "http://$ip`:8000/exec" -Method POST -Body $script `
     -ContentType 'text/plain' -TimeoutSec $timeout -UseBasicParsing).Content
}
Invoke-Guest 'cd ~/sbx2/linux-sandbox && ctest --test-dir build --output-on-failure 2>&1 | tail -30'
```

虚拟机上的工作树在 `~/sbx2/linux-sandbox`。推送改动：

```powershell
$bytes=[IO.File]::ReadAllBytes("$clone\src\runtime\run_session.cpp")
Invoke-WebRequest -Uri "http://$ip`:8000/put/patch/run_session.cpp" -Method POST `
  -Body $bytes -ContentType 'application/octet-stream' -TimeoutSec 60 -UseBasicParsing
Invoke-Guest 'cp ~/sbxctl/patch/run_session.cpp ~/sbx2/linux-sandbox/src/runtime/ && cd ~/sbx2/linux-sandbox && cmake --build build --parallel 2 2>&1 | tail -2'
```

**服务重启后它就没了**，需要用户在虚拟机里粘一次启动命令（见仓库外的 `netctl/server.py` 说明，或重新给用户那段 `srv.py`）。

### 用户在虚拟机里的一键验证

```bash
bash verify.sh --clean      # 配置 + 构建 + ctest，输出直接在终端
```

## 3. 设计范围与完成度

原始设计要求：把沙箱当作独立执行系统，重新设计安全边界、执行流程、类型模型、资源管理、故障处理。**不涉及**多 Agent、常驻管理服务、数据库。

| # | 条目 | 状态 |
|---|---|---|
| 1 | 业务层全类型化，JSON 只在配置/CLI/持久化适配层 | **基本完成**。请求与存储都已类型化；`session_wire` 请求方向已类型化，**回包方向仍渲染成 JSON**（理由见 §4.3）；`SessionStore` 的落盘是 `record_json` |
| 2 | 单次与会话共用一套执行核心 | **完成**。会话每条命令一个 `ExecutionCore`，结果直接是 `ExecutionResult`；`CommandRecord`/`CommandFacts` 不再参与运行时 |
| 3 | outcome / output / cleanup 三分 | **完成**。`verdict` 与 `summary` 分开，清理失败走 125 而 `summary` 仍可 success |
| 4 | 进程与 FD 所有权明确 | **完成**。17 种控制消息、`fd_contract`、收发两侧强制校验 |
| 5 | 隔离准备步骤化 + 逆序回滚 | **部分**。单次与会话**都已接 `PrepareSequence`**（会话的失败原因与回滚失败分开记录）；策略身份已进结果，但策略本身仍是兼容型拒绝列表 |
| 6 | 取消/超时/后代清理统一停止流程 | **完成**。停因唯一入口 + 停止阶梯 + 有界预算 |
| 7 | 输出有界、语义清晰 | **完成**。EOF / 写入完成 / 落盘三件事分开 |
| 8 | workspace 与 runtime 边界 | **完成**。`keep_on_close`、`/input` 的 `snapshot_copy`、复制路径的 `admit()`、**隔离模式下 `/output` 一律是 tmpfs 配额（会话累计；单次执行与批量任务在运行结束时一次性导出）**、`/transfer` 双向移交与三类结果分开报告均已生效 |
| 9 | 持久化记录独立建模 | **完成**。`SessionStore` 接口只收 `AcceptedCommand` / `FinishedCommand`，落盘走 `core/record_json`；旧 v2 记录仍可读回；SDK 直读 `result.json` 时按记录格式映射 |
| 10 | 围绕故障边界的测试 | **完成**。截断/损坏/乱序/超时/取消/后代残留；连续会话 100 条命令的 FD/子进程/内存增长有界 |

## 3.5 语义澄清（静态评审后）

- **命令终态只有一个来源：`ExecutionCore`**。控制器只喂事实（`stop intent`、`main exited`、
  `descendants empty`、`output eof`、`output finished`、`tick`）并执行它给出的动作；它不自己
  推导超时、取消或成功判定。
- **导出期间的阻塞**：init 同步复制目录，复制期间控制通道响应变慢。监督端有等待上限
  （默认 30s，`SBX_TRANSFER_WAIT_MS` 可调），超时后记 `TRANSFER_TIMEOUT`；此时复制可能仍在
  进行，因此**通道在本会话内作废**，后续传输快速失败，直到会话关闭。迟到回执按序号丢弃：
  等待期间由 `await_transfer()` 丢，等待放弃之后由主循环丢（记一条
  `stale transfer reply discarded`）——**它绝不是"意外消息"**，把迟到回执当成协议错误会
  杀掉一个健康的会话（本轮真机验收就踩到了）。同一时刻只允许一个在途传输。
- **配额计量不是同一种量**：tmpfs 的 `size`/`nr_inodes` 是内核记账（含目录项与元数据），
  `WorkspacePolicy.max_bytes/max_entries` 是复制层自己数的文件字节与条目数。tmpfs 占用
  **宿主内存**，受 cgroup `memory.max` 约束：配额设得比内存上限大时，先撞的是内存限制。
- **工作区持久性**：会话里命令结束**不自动导出**，只有 `manifest`/`export`/关闭时才移交；所以 init
  异常退出最多丢失"上一次成功移交之后"的写入。单次执行（决策 A 之后）**在目标结束后、init 退出前
  必定导出一次**，监督端会等到这次答案才收尾（上限默认 30s，`SBX_TRANSFER_WAIT_MS` 可调）；
  导出失败或 init 在导出前死亡 → 结果里 `workspace.export.ok=false`、`verdict=harness_error`，
  **不因为目标退出 0 就报成功**。`keep_on_close` 管的是宿主已发布副本。
- **等导出不等于失去响应**：监督端在 `await_transfer()` 里等待沙箱复制时仍然看信号（SIGINT/SIGTERM）
  和会话关闭期限，收到停止就把 `Stop{}` 转给 init（`InterruptOnStop` 会让复制以
  `TransferAborted` 结束），并且**记住**这次停止，等等待退栈后由主循环 `close("signal")` 收尾。
  所以"复制可被中断"现在从外部按 Ctrl-C 就能触发，而不是只有单元测试里注入回调才能看到。
  客户端发起的第二次请求仍要等这次导出结束（服务是一问一答），这一点是协议边界，不是遗漏。
- **原子可见 ≠ 持久化，而且这是两类事实**：`renameat2(RENAME_EXCHANGE)` 只保证交换瞬间可见。
  `publish_tree_over()` 返回 `PublishResult{published, durability_confirmed, durability_error,
  cleanup_error, retained_previous_path}`：
  - 交换前失败 → 抛异常（什么都没发布）；
  - 交换后 `fsync` 父目录失败 → `published=true`、`durability_confirmed=false`，并且**旧副本故意保留**
    （它是唯一确定在盘上的版本，回收掉会让崩溃后两个版本都没了），**保留位置写进
    `retained_previous_path`**——跨文件系统时那是生成的暂存目录名，调用方自己推不出来；
  - 交换并确认持久、但旧副本回收失败 → `published=true`、`cleanup_error` 非空。
  两个调用方各自记录这些事实：单次执行与会话都写进 `workspace.export.durable/durability_error/
  retained_previous_path`，只有回收失败才进清理列表。**持久性未确认不算"未交付"**（产物是可见的），
  也不改 `verdict`，但报告必须让人读得到；`ok` 只表示"当前可见"，不是断电持久性保证。
- **安全策略强度**：`security.policy=baseline` 仍是兼容型拒绝列表，`security.policy/version`
  只用于标识与可追溯，**不是强度声明**；补上配额不扩大这个承诺。

### 3.6 三段"完成"：谁负责、谁读

"完成"在这套代码里有三个不同的意思，混起来读会把收尾阶段的失败当成命令失败。
`finished()` 的名字保留，但它只表示第一段。

| 阶段 | 事实在哪 | 负责方 | 谁读它 |
|---|---|---|---|
| **命令监督**完成 | `ExecutionCore::finished()` / `Phase::finished` / `Completed{}` 动作（`apply()` 里是 no-op） | `ExecutionCore` | 控制器：停止喂事实，进入收尾 |
| **环境收尾**完成 | launcher 已回收、cgroup 已删除、workspace 已交付或明确未交付 | 控制器的 `finalize()` / 会话收尾 | 报告：`cleanup`、`workspace.export` |
| **记录落盘**完成 | `result.json`（单次执行）/ 会话记录已写 | 控制器（`store_->report` / `records_->close`） | 调用方：CLI、SDK、批次 |

三条边界因此各有归属：核心判定目标结局，控制器判定环境是否收干净，报告层只做适配。
报告里的 `summary`（目标做了什么）/ `verdict`（环境可不可信）/ 退出码就是按这三段分的。

## 4. 剩余工作（按建议顺序）

每项都遵循同一条纪律：**改完 → 虚拟机跑全套 → 绿了才提交**。中途不提交、不改 D 盘。

### 4.1 `SessionStore` 迁移到类型化记录（**已完成**，不要再做一遍）

落盘形状（`record_schema_version = 4`；可读下界 `record_schema_minimum_readable = 3`，与 `schema_version = 2` 的会话快照文件是两套版本）：

```
cmd-N/request.json   encode(AcceptedCommand)   index / request 意图 / accepted_at_ms / effective_cwd
cmd-N/result.json    encode(FinishedCommand)   index / result{outcome,stdout,stderr,cleanup,timing} / finished_at_ms
```

- 回包**仍然是旧的会话 JSON**（`state` / `exit_code` / `stop_reason` / `stdout.seen`…），由 `session_store.cpp` 的 `terminal_reply()` 从记录生成；协议版本和存储版本因此互不影响。
- `effective_cwd` 单独存放，不能折进 `request.cwd`：`same_intent()` 把「有没有 cwd」也算作意图，折进去会让重放误报 `REQUEST_CONFLICT`。
- `Cancelled` 增加了 `cause`（requested / session_closed），`StartupFailed` 增加了 `exec_errno`。这两个字段是为了让 `stop_reason` 与 `exec_errno` 在类型化之后仍然可还原，`sessions.py` 断言了它们。
- 单个命令的 `metrics` 不再进记录（`ExecutionResult` 里没有这个位置），会话级指标仍在快照里。
- SDK 的 `wait()` / `result()` 直读 `result.json`，所以 `sdk/session_client.py` 里有一个 `_record_reply()`，把记录映射成回包形状；旧形状原样返回。
- 恢复语义：只有「接受记录」存在才算跑过；缺结果时写一条 `Indeterminate` 记录（回包里显示 `interrupted`），不会重跑。

### 4.2 `/input` 快照复制（**已完成**）

- `workspace.input = "snapshot_copy"` 时，`snapshot_inputs()`（`src/storage/artifacts.cpp`）在 launcher 启动前把每个 `/input` 挂载点的宿主目录复制到 `<run>/input`，并把 `RunConfig.mounts[].source` 改指向副本。之后下游（launcher、init、mount）只看到副本。
- 复制**复用** `file_request` 的 snapshot 路径，因此继承同一套语义：祖先目录不跟随符号链接、拒绝符号链接/硬链接/特殊文件、边读边校验文件未被修改、staging 目录 + `renameat2(NOREPLACE)` 原子发布。失败时保留 staging 目录用于诊断，不降级成绑定。
- 单次执行里它是 `PrepareStep::input_snapshot`（插在 `output_capture` 与 `launcher` 之间，因为 launcher 拿的就是挂载源），撤销时删除副本；会话路径在 `RunStore` 之后、`LauncherClient` 之前调用，关闭时随 workspace 一起删除，删除失败追加进 `cleanup_errors`。
- 验证边界：process 模式不允许 mounts，所以 `/input` 只有 isolated 模式才存在。隔离路径需要一个可委派的 cgroup 和放开 `kernel.apparmor_restrict_unprivileged_userns`；`tools/isolated-accept.sh` 把这个过程（含按原值恢复 sysctl、跳过即判失败）都包住了。
- **已全部验到**（同一台机器，用户临时 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` 之后跑
  `tools/delegated_run.py --test-runtime ...`）：`isolated integration passed`、
  `isolated session smoke passed`、`isolated /input snapshot passed`、`isolated batch smoke passed`，
  服务 `code=exited/status=0`。即副本确实被挂进沙箱 namespace，且沙箱内看不到宿主后续修改。
  该 sysctl 不持久，重启后再跑需要重新放开；项目自身不改宿主策略。
- 不放开该开关时的宿主侧证据（复制、原子发布、改写挂载源、按 `keep_on_close` 保留/删除）：见下。
- **不依赖 namespace 也能验的部分**（复制发生在 launcher 之前、纯宿主侧）：用真实 config 跑 `sandbox session-open`，隔离如预期在启动阶段失败，但 run 目录里
  - `config.json` 的 `/input` 挂载源已被改写为 `<run>/input`；
  - `<run>/input/data` 与 `<run>/input/sub/deep` 内容与宿主一致（含子目录）；
  - 之后改宿主源文件，副本仍是原内容；
  - `keep_on_close=true` 时副本与 workspace 一起保留；`false` 时两者都被删除，且 `cleanup_errors` 为空。

### 4.3 `session_wire` 类型化（**请求方向已完成，回包方向待做**）

已完成：

- `decode_session_request(packet)` 直接把 Protobuf 解成 `SessionRequest`，中间不再经过 JSON。原 `decode_execute` / `decode_stdin` / `decode_request(→Json)` 三个解码器合并成一个。
- 控制器只收一个处理函数：`ControlServer::serve(std::function<Json(const SessionRequest&)>)`。原来 execute/stdin 的旁路回调（`execute_handler` / `stdin_handler`）删掉了——`SessionRequest` 本来就覆盖它们。
- `session_request_to_json()` 只是**渲染**，供 CLI 与 interop bridge 使用；`decode_request()` = 解码 + 渲染，不再是第二个解码器。
- **契约收紧（有意为之）**：门的这一侧现在按业务类型校验，所以 `cwd: ""`、`wall_ms: 0`、`cmd-01` 这类值在解码处就返回 `INVALID_REQUEST`，不再走到控制器里才被拒。`session_wire_tests.cpp` 与 `session_wire_interop_tests.py` 的相关 fixture 从"非法值"改成"合法值"，并新增了拒绝断言——断言的是同一件事：两个门对同一个请求给出同一个结论。
- 批处理控制通道保留自己的 JSON 协议：`serve(typed, json_handler)` 的第二个参数就是给它和 `accept_cancel` 用的。

附加（回包方向，已完成的部分）：

- `SessionStore` 不再渲染回包：`replay` / `request_status` / `result` / `lookup_request` 返回类型化的 `CommandReport{accepted, optional<FinishedCommand>}`，`recover()` 返回 `SessionRecovery{session, last}`。**存储层从此不知道回包长什么样。**
- 回包词汇表搬到了 wire 层：`session_wire::command_report_to_json()`。"accepted 但没结果"（`uncertain`）、"正在运行"（控制器用活的 `current()` 回答）、"终态"（`finished`/`interrupted`）是同一个 `CommandReport` 的三种说法，选择权在协议层。
- 回包 payload 本身仍是 JSON，这是**有意的停点**而不是没做完：`status`、`files`、以及"正在运行的命令摘要"都带 `metrics` 这类可扩展观测 payload，业务类型里没有它的位置；给 `SessionReply` 加一个 `RawReply{Json}` 再把这些包回去，只是把 JSON 换个名字，还要多维护一套 `session_reply_to_json`。真要往下做，先设计"观测数据在业务类型里怎么表达"，而不是先打字化。
- 顺带：`CommandReport` 让存储测试从"断言回包里的字符串"变成"断言记录里的字段"（`outcome.exit_code()`、`std::get_if<StartupFailed>()`），渲染由 `session_wire_tests` 覆盖。

### 4.4 会话控制器接入执行核心（**已完成**）

做法与关键差异（改之前读一遍，别重复踩）：

- 每条命令一个 `ExecutionCore`，`start()` 时构造并立刻 `env_ready()`（环境在会话级已经准备好），所以命令直接进入 running，不经过 startup 阶段。
- init 的一条 `CommandDone` 同时携带**三个事实**：目标结束、后代集合为空、以及"是我们让它停的"。喂给 core 的顺序是：先 `stop_requested(原因)`，再 `main_exited`，最后 `descendants_empty`。顺序反了就会把"我们发的 SIGTERM"当成目标自己的死因（`sessions.py` 断言 cancel 必须报 `cancelled`）。
- **动作映射与单次执行不同，这是两者的真实差异**：
  - `SignalTarget` → 给 init 发 `CancelCommand`（会话的进程组归 init 管）。
  - `KillTarget` → 会话没有"只杀这条命令"的手段（cgroup 是整个会话的），所以升级动作是 `close("command_supervision_timeout")`，由 init 统一收场。
  - 命令已被 init 报告结束（`execution_->done`）时，`SignalTarget` 直接丢弃：此时再发 cancel 会打到**下一条**命令上。这是刻意的，不是漏改。
  - 会话的 `grace` 取 `grace_ms + 5000`，让 init 自己的升级先走，core 不抢在它前面。
- 命令结果直接来自 `core->result()`，`FinishCommand` 落盘后回包由 `SessionStore` 渲染；`outcome_of(facts)`/`finished_of(record)` 两个转换函数删除。
- 顺带修好的两处类型缺口：`start_failed(reason, exec_errno, now)` 让 `exec_errno` 能进 `StartupFailed`（`sessions.py:87` 依赖它）；`StopIntent::session_closed` 现在产出 `Cancelled{cause=session_closed}`，不再是丢掉原因的裸 `Cancelled`。
- 每条命令的 `metrics` 计算块删除：自从 4.1 把 per-command metrics 移出记录，它已经无处可去。

### 4.5 清理 `CommandRecord` / `CommandFacts`（收尾，见 MIGRATION_SESSION 第 3 步）

## 5. 已经踩过的坑（别重犯）

### 构建

- `sandbox_model` 必须链接 `sandbox_core`（`config.cpp` 调用 `WorkspacePolicy::validate()` 等）。`cross-verify.ps1` 查不出来，因为它是全对象一起链接
- 往 CMakeLists 插入块时注意结尾换行，否则 `endforeach()` 会和下一条命令粘成一行，`cmake` 直接 parse error
- 仓库必须保持 LF：`core.autocrlf=false`，脚本 CRLF 会导致 `bash` 报奇怪的错
- **磁盘上有 12 个文件是 CRLF、索引里是 LF，而 `git status` 报干净**（索引 stat 缓存挡住了，`git ls-files --eol` 显示 `w/crlf`）。改动它们之后直接 `git add` 会把 CRLF 写进 blob，静默破坏上面那条 LF 纪律。名单：`include/sandbox/storage/store.hpp`、`src/storage/run_store.cpp`、`src/runtime/session_protocol.cpp`、`src/application/service.cpp`、`src/platform/environment.cpp`、`sdk/sandbox_client.py`、`sdk/session_client.py`、`third_party/spdlog/{INSTALL,LICENSE,README.md,.gitignore,cmake/spdlogConfig.cmake.in}`。判别方法：`git hash-object <file>` 与 `git ls-files -s <file>` 的 blob 不一致而 `git status` 干净。提交前先 `-replace "\r\n","\n"` 归一，`git diff` 就只剩真实改动

### 存储

- **两套 `schema_version` 不要混**：会话快照 / artifacts 是 `2`（`SessionStore::write()` 负责盖章），命令记录是 `record_schema_version = 4`（由 `encode()` 自带；解码接受 3..4，新增字段一律可选）。记录文件必须走 `store_.write_json(name, encode(record))`，走过 `write()` 会被改成 2，然后被 `read()` 当成坏记录
- 回包形状和存储形状是两件事。改记录结构时，`session_store.cpp` 里的 `terminal_reply()` 和 `sdk/session_client.py` 里的 `_record_reply()` 必须一起改，否则「主机活着」和「只有文件」两条路径会给出不同答案

### 运行时

- **SIGTERM 是取消信号**：`timeout` 默认发 TERM，会触发沙箱的取消流程而不是杀掉它。压测时用 `timeout -k 1 8`，否则进程杀不掉
- **`pkill -f "build/sandbox"` 会杀掉自己**（自己的命令行里含这个字符串），用 `pkill -x sandbox`
- 对端（launcher/init）退出时可能给 `ECONNRESET` 而不是干净 EOF，按通道关闭处理，不要当宿主错误
- **`INIT_DONE` 可能丢失**：init 已退出、通道已关闭、进程已回收，但这条消息没被读到。通道关闭意味着"证据耗尽"，停止阶段应据此收尾。这是之前间歇性挂死的根因
- `supervisor_failed` 在停止阶段是空操作，不能指望它兜底；停止阶段另有 2 秒预算（`stop_budget_ms_`）
- spdlog 的 `info` 会缓冲，进程被杀就丢；诊断日志用 `warn` 才会落盘

### 调试

- `SBX_SUPERVISE_TRACE=1` 会让监督循环每 500ms 记录 `phase` 与各确认标志，挂着不动时先开它
- gdb 附加被 `yama/ptrace_scope=1` 挡住（不是 root），trace 是更可行的路
- 虚拟机 `isolation.tools.hgfs.disable = "TRUE"`，共享目录不可用；主机非管理员，开不了防火墙入站端口。所以通道是"虚拟机当服务端、主机连它"

- **`systemd-run` 不继承调用者的环境**：`export FOO=x; systemd-run --user ...` 里 `FOO` 是**不存在**的。
  要把目录/开关传给单元里的进程，必须用参数显式带进去（本仓库走 `delegated_run.py --artifacts`），
  或者 `-E`。我第一次写"失败会保留会话现场"时没验证这条，结果那行日志指向一个从未创建的目录。
- **"1ms 超时"不是确定性测试**：复制快时根本不会超时。要测超时路径，让预算取 **0**（"完全不给时间"），
  否则用例是抛硬币。同理，任何依赖"拷贝足够慢"的断言都不可信。
- **能力在 fork 之前就丢了**：`prepare_environment()` 里 `drop_capabilities()` 之后，目标子进程没有
  `CAP_SYS_ADMIN`，任何"给目标建独立 mount namespace 再遮蔽某个路径"的方案都会 EPERM 失败。
  想让目标看不到某个目录，正确做法是**根本不要让它出现在文件系统里**（fd + `*at`），而不是遮挡。
## 6. 关键代码位置

| 关注点 | 文件 |
|---|---|
| 执行核心状态机 | `include/sandbox/core/supervisor.hpp`、`src/core/supervisor.cpp` |
| 单次执行适配 | `src/runtime/run_session.cpp`（`supervise()` 循环、`apply()`、`finalize()`） |
| 会话控制器 | `src/runtime/interactive_session.cpp`（`handle(SessionRequest)`、`event()`、`start()`） |
| 业务类型 | `include/sandbox/core/ops.hpp`（`SessionOperation` 变体）、`request.hpp`、`result.hpp`、`outcome.hpp` |
| 协议 | `include/sandbox/core/control.hpp`（消息 + `fd_contract`）、`src/runtime/protocol.cpp` |
| 准备与回滚 | `include/sandbox/core/prepare.hpp`、`src/core/prepare.cpp` |
| workspace 边界 | `include/sandbox/core/workspace.hpp`、`src/core/workspace.cpp`、`src/storage/artifacts.cpp` |
| 持久化记录 | `include/sandbox/core/record.hpp`、`record_json.hpp` |
| 报告与退出码 | `src/model/report.cpp`（`verdict` / `summary` / `cli_code`） |

## 7. 一条纪律

这个项目之前的绝大多数回归，都来自"改了字段含义但没走完所有读取方"，以及"以为某条路径会执行、其实被前面的赋值覆盖了"。每次改动前先回答两个问题：

1. 这个字段还有谁在读？
2. 我写的这个覆盖，在它想覆盖的那行之后吗？

绿基线比新问题重要。任何一步把 33/33 弄红的改动，都应该当场回退，而不是"再改改就好了"。

### 4.6 syscall 策略身份进执行结果（已完成）

`ExecutionResult::security`（`SecurityPolicyFacts{name, version}`）是"目标到底跑在哪个策略下"这个事实：
`SecurityPolicy` 里 `baseline_rule_set_version` 与规则表放在一起，改规则就必须改版本——两次结果都写
"baseline" 并不代表可比。空 name 表示没有装过滤器（与"装了默认策略"不是一回事）。
落盘进 `result.security`，单次执行的报告里也有一份；会话回包没有这个字段，因为
`proto/session_v1.proto` 里没有位置，加它要重生成 Python pb2。

### 4.7 单次执行与批量的 `/output` 配额（决策 A，已完成）

语义：隔离模式下 `/output` **一律**是沙箱 namespace 里的 tmpfs（`size=workspace.max_bytes`、
`nr_inodes=workspace.max_entries`），`mounts[target=/output].source` 从"工作区本身"变成
**运行结束时最后一次导出落地的宿主目录**。批量任务的子进程就是 `sandbox run`，因此自动继承；
批次按子运行的 `verdict` 判定，所以"产物未交付"会让任务失败。

接线（一处发送，一处接收，和会话同一套消息）：

| 位置 | 做什么 |
|---|---|
| `src/runtime/run_session.cpp::prepare()` | 有 `/output` 挂载时：把 source 记成导出目的地、把它**从 `config_.mounts` 移除**、建 `<run>/transfer` 与 `transfer/in`（`PrepareStep::state_root`，可回滚）、置 `config_.workspace_transfer` |
| `run_session::on_launcher`（`InitCreated`） | 发完 `Prepare{}` 后**以 fd 发** `TransferChannel{}`，宿主目录在沙箱里没有路径 |
| `src/runtime/init.cpp::run_once` | `prepare_environment` 之后、`EnvReady` 之前收 fd；`supervise_command` 返回后（且**确实 exec 过**）`export_workspace(fd,"final")` → `Transferred` → `InitDone` |
| `run_session::on_init`（`Transferred`） | 记 `report_.workspace`，`renameat2(RENAME_EXCHANGE)` 把 `transfer/final` 与宿主目的地交换（跨文件系统时先拷到目的地同盘再交换），旧副本交换后才回收 |
| `run_session::supervise()` | 目标结局已定但导出未答：**不结束**，等到答案或 30s 预算（`SBX_TRANSFER_WAIT_MS`），否则报告"未交付" |
| `run_session::finalize()` | `attempted && !ok` → `errors` + `summary` 不再是 `success` + `verdict=harness_error`（`cleanup_incomplete` 优先） |

报告形状（`result.json`，**报告 schema_version 从 1 升到 2**；命令记录仍是
`record_schema_version = 4`，记录形状没变）：

```json
"workspace": {
  "mode": "tmpfs",
  "quota": {"max_bytes": 65536, "max_entries": 4096},
  "export": {"attempted": true, "ok": true, "bytes": 10, "entries": 1, "error": "",
             "durable": true, "durability_error": "",
             "retained_previous_path": ""}
}
```

`export` 是**三个独立事实**，不是一个布尔：`ok` = 新树已经在宿主目的地可见（**只是可见，不是断电持久**）；
`durable` = 这次可见已经落盘；`error` = 根本没交付成功。`durable=false` 时旧副本被保留在
`retained_previous_path`（宿主路径，通常在 `<run>/transfer/` 下），崩溃后可能回到它。
回收旧副本失败只进 `cleanup.errors`，不会伪装成交付失败。

没有 `/output` 目的地的运行（process 模式，或隔离但没有 `/output` 挂载）写作 `null`：
读侧必须把"没有这个字段/为 null"当成**未知**，不能当成"什么都没写"。
`attempted` 只在**目标确实 exec 过**之后才为真：启动失败或准备失败没有产物可交，不该报未交付。

失败用例怎么造：`tests/acceptance/isolation.py` 用 fixture 的 `link-output` 在 `/output` 里放一个符号链接
（导出策略拒绝符号链接），目标本身退出 0 —— 这样"目标成功但产物未交付"是确定性的失败，
不依赖改宿主权限或刚好撞上磁盘满。配额用例用同一个 `fill-bytes` 探针，超预算必须拿到 `60`。

**第一次上真机踩到的两个坑（都已修，别退回）**：

1. **init 带着未读帧关闭控制通道 → 对端 `ECONNRESET`，已发出的回执被内核丢弃**。目标退出后
   监督端会发一个 `Stop`（`main_exited` 的 `SignalTarget`），而 init 这时已经在导出、不再读通道；
   init 关闭 socket 时 `Stop` 还排在它的接收队列里。Linux 的 AF_UNIX 在这种情况下会给对端
   `ECONNRESET` **并且丢掉对端接收队列里已有的帧**（socketpair 最小复现：
   `['ERR:Connection reset by peer']`，先抽干再关则是 `['A','B','EOF']`）。现象就是"目标成功、
   导出其实已落盘、报告却说未交付"。
   两层修法：init 在发回执前和发完后都 `discard_pending()` 抽干通道；监督端不只信消息——
   `<run>/transfer/final` 已经在盘上就 `adopt_landed_export()` 直接采用并原子发布
   （导出是先 `renameat2` 再回执，所以盘上的树比消息更可信）。
2. **OOM group kill 会把 init 一起杀掉**（实测 `oom_kill=3`、`oom_group_kill=1`）：namespace 里的
   tmpfs 随 init 一起消失，产物**真的**交付不了。这不算 bug，报告必须写"未交付"
   （`workspace.export.ok=false` + `errors`），`summary` 仍是 `oom`、`verdict=harness_error`。
   所以 `tests/acceptance/isolation.py` 的 OOM 用例是 `export_ok=None`（两种结果都接受，但都要求有据可依），
   其余用例仍强制 `ok=True`。别把这条改成"OOM 也必须交付"。

### 4.8 静态评审后的四项收尾（已完成）

| 项 | 内容 |
|---|---|
| 等待传输期间的响应性 | `await_transfer()` 每轮先看信号：收到 SIGINT/SIGTERM 就把 `Stop{}` 转给 init（复制以 `TransferAborted` 结束），并置 `signal_pending_` 让主循环仍能 `close("signal")`；同时检查关闭期限。**红线**：不能在这里 `server_->serve()`——`sync_workspace` 会在 `handle()` 内部被调用，递归服务会坏。客户端发起的第二次请求仍等这次导出结束，这是协议边界 |
| 端到端停止用例 | `tests/acceptance/workspace_quota.py` 第 6 条：15000 个条目、导出在后台线程、100ms 后给沙箱进程发 SIGINT，断言 5s 内结束、`stop_reason=signal`、导出报 `TRANSFER_INTERRUPTED`。必需行已进 `isolated-accept.sh` |
| 类型化停止原因 | `Transferred` 增加 `TransferStop{none,interrupted}`，`init` 捕获 `TransferAborted` 填它；`interactive_session` 不再用 `error.find("interrupted by a stop request")` 猜类型 |
| 发布逻辑去重 | 两个调用点合并成 `publish_tree_over()`（`src/storage/artifacts.cpp`）：交换（跨文件系统时先拷到同盘再交换）→ `fsync` 两侧父目录 → 回收旧副本 |
| 发布结果三分 | 返回值改为 `PublishResult{published, durability_confirmed, durability_error, cleanup_error, retained_previous_path}`：交换前失败抛异常；交换后持久性失败**保留旧副本**、把保留位置一起返回；回收失败是单独的第三类。两个调用方都写进 `workspace.export.durable/durability_error/retained_previous_path`，不再把持久性失败塞进清理列表。`ok` 只表示"当前可见" |
| 发布测试的覆盖范围 | `publish_tests.cpp` 覆盖：交换成功、目标缺失抛异常、持久性未确认、回收失败；跨文件系统（`/dev/shm` 与 `/tmp` 不同设备时才执行）。**持久性未确认是用"父目录不可读"制造的**（交换只需写+执行，同步需要读），不是模拟 `fsync()` 返回 I/O 错误；内核级 I/O 错误路径、以及跨文件系统分支里的那次提前返回都没有被覆盖。为了让"通过 ≠ 跑过"可核对，`publish_tests` 会打印**明确的一行**：`cross-filesystem publish: exercised (...)` 或 `... NOT exercised (...)`，`ACCEPTANCE` §2/§7 要求验收记录把这行抄下来 |
| 验收脚本的信号 | `isolated-accept.sh` 的恢复只挂 `EXIT`，INT/TERM 改为 `exit 130/143`；此前信号到达后脚本会带着已恢复的 sysctl 继续跑 |

一处仍然保留的有意取舍：`init` 的单次执行导出**不轮询控制通道**（回调传空），超时由监督端的
预算兜底；会话路径才在等待期间处理信号。两者语义不同是因为单次执行没有"后续命令"要保护。

### 4.9 接口与异常安全收紧（已完成，设计评审第三轮）

| 项 | 内容 |
|---|---|
| 结果只读 | 删掉 `ExecutionCore::result()` 的非 const 重载；写入口只剩 `mark_launcher_reaped` / `mark_cgroup_removed` / `mark_workspace_removed` / `add_cleanup_failure` / `set_security`。"结果由状态机形成"因此是编译期事实，不是纪律 |
| 准备步骤契约 | 写进 `prepare.hpp`：`apply` 必须异常安全（框架只 undo 已完成的步骤）；`undo` 可假定 `apply` 返回过。**允许的残留**限定为运行目录内的诊断文件（`stdout.bin` / `stderr.bin` / `internal.log` / `state.json` / `config.json` / `control.sock` / 半成品 `transfer/`、`input/`）；**绝不允许**留下 cgroup、挂载、进程、运行目录之外的套接字或目录 |
| 目录事务 | `create_directories_or_remove()`（`src/platform/linux.cpp`）：只回收**本次成功创建**的目录、预存目录复用不删、逆序 `rmdir`（不递归）、保留原始失败原因并在后面追加回滚删除失败的诊断。两个 `state_root` 步骤与 `create_transfer_layout()` 都改用它。`output_capture` 核对结论：FD 由 RAII 关闭，已创建的 `stdout.bin`/`stderr.bin` 属于允许的诊断残留，不算泄漏 |
| 撤销不短路 | `remove_all_collect()`（同文件）：逐个尝试删除、收集全部失败后一次性报告。会话 `state_root`（`rootfs_` / `workspace` / `transfer_`）与 `input_snapshot` 的 undo 改用它——此前第一个删除失败就抛，后面的目录根本不会被尝试，与 `PrepareSequence`"一次回滚失败不阻断其他回滚"的规则不一致 |
| 完成语义 | 见 §3.6 的三段划分；`finished()` 名字保留 |

测试覆盖（`tests/integration/publish_tests.cpp`）：目录事务的三段场景（自建目录被删、预存目录保留、预存文件保留、错误信息带原始原因）+ **真实调用路径** `create_transfer_layout()`（占用 inbox 时失败且不动预存目录、全新目录可创建、重复调用幂等）+ `remove_all_collect()`（一个删不掉时其余仍然被尝试、失败信息带路径）。会话 `state_root` 的 undo 本身由 ctest 的 `session_prepare` 覆盖（它注入 launcher 步骤失败并要求前面的步骤被撤销）。
**没有覆盖的**：`rmdir` 回滚本身失败的诊断分支——那需要让一个刚创建的空目录在回滚前变成非空，测试进程控制不到，代码里保留但不谎称测过。

### 4.10 迟到回执不是协议错误（真机验收发现，已修）

`tests/acceptance/workspace_quota.py` 的"超时后通道作废"用例在真机上红过一次：第二次 `export` 没有拿到
`TRANSFER_UNUSABLE`，而是会话已经死了。根因是**等待放弃之后没人再按序号丢弃迟到回执**——

- `await_transfer()` 内部有 stale-reply 处理，但它在超时那一刻就抛出了；
- 复制仍在沙箱里跑，几毫秒后那条 `Transferred` 到了，被主循环的 `drain_channels()` 读走；
- 会话的消息分派只认 `EnvReady` / `CommandDone` / `InitError` / `InitDone`，**没有 `Transferred`**，
  于是按"意外消息"抛出，把一个健康会话判成 `host_error`。

修法：主循环把 `Transferred` 当成迟到的回执丢掉，并记一条
`stale transfer reply discarded (sequence N)`（与 `await_transfer()` 里同一个标志，只记一次）。
用例里补了回归断言：超时 → 等 0.5s 让迟到回执真的到达 → 会话仍能执行命令。

顺带修了 `workspace_quota.py` 的失败现场保留：它把会话状态根放在 `TemporaryDirectory` 里，而
`sys.excepthook` 在临时目录清理**之后**才跑，所以以前只打印"kept session state"却什么都没留下。
现在状态根在临时目录之外，失败时真能拿到 `result.json`。

**同一类问题在会话关闭路径上还有一份**（下一轮真机验收抓到）：`close("signal")` 也会给 init 发一个
`Stop`，而 init 这时已经发完 `INIT_DONE` 正在退出；它带着未读的 `Stop` 关闭 socket，内核给监督端
`ECONNRESET` 并丢掉还在队列里的 `INIT_DONE`。监督端于是走到 `drain_channels()` 的
`if (!(parent ? reaped_ : done_)) throw;`，把**一次主动关闭**报成 `host_error`——`stop_reason`
从正确的 `signal` 被覆盖成 `host_error`，`errors` 里只有一句 "control channel closed"。
两处修法，和单次执行那次完全对称：

1. `init` 在被打断的导出路径上发完 `INIT_DONE` 之后也 `discard_pending()`，先吃掉那个迟到的 `Stop`；
2. 监督端在 `stopping()` 期间接受"通道先于 `INIT_DONE` 关闭"：通道关闭**结束证据**，不算失败
   （把 `done_` 置真）；不在停止期间关闭仍然照旧判错。

顺带补齐了两件诊断缺口：

- **会话现在也写 `internal.log`**（`<run>/internal.log`，与单次执行同一个文件、同一套等级规则），
  记录每个控制事件的种类与方向、通道关闭时的 `reaped/done/stopping/quarantined` 标志、`close(reason)`、
  信号与导出结果。此前会话只留一个 `result.json`，判断"哪个通道先关"只能靠猜。
- **`execution_ms` 单位错**：`session_wire` 把微秒直接写进毫秒字段，83 ms 的命令报成 `83000`。
  现在按毫秒截断。