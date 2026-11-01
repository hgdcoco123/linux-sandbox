# 交接文档：沙箱内部重构

写给下一个接手的会话。读完这份就该知道：改到哪了、怎么验证、还剩什么、哪些坑已经踩过。

## 1. 现在的状态

| 项 | 值 |
|---|---|
| 仓库 | `D:\cpp_project\linux-sandbox`（工作目录唯一权威） |
| 提交 | `4d1c802`，工作区干净；本轮在 `ec5e441` 之后落了 7 个提交（见 §1.1） |
| 测试 | **32/32 通过**，Linux 7.0 内核实机；每项改动后连跑 4 次稳定，最后一次还做了独立目录的全新构建（`build-clean`） |
| 跳过项 | `isolation_integration`（无 cgroup 委派时的预期行为，不是失败） |

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

工作副本在 `C:\Users\lipinzheng\documents\deepseek-harness\default-workspace\sbx`（同一个 git 仓库的 clone，分支 `master`）。

**同步方式**：改在 clone 里 → `git commit` → `git -C D:\... fetch <clone> master && git merge --ff-only FETCH_HEAD`。不要直接改 D 盘，也不要整目录覆盖。

## 2. 怎么验证

### 主机侧（只能跑通纯逻辑）

```powershell
powershell -ExecutionPolicy Bypass -File tools\portable-tests.ps1
powershell -ExecutionPolicy Bypass -File tools\cross-verify.ps1 -Zig <zig.exe> -HostWork <work>
```

`cross-verify.ps1` 做 8 段检查：9 套可移植测试 → profiles → CMake 源列表一致性 → 42 个 TU 编译 → 23 个测试 TU 编译 → 2 个二进制链接 → 23 个测试链接。**它不跑 CMake 的库边界链接**，所以像 "`sandbox_model` 没链接 `sandbox_core`" 这类错误它查不出来——那是第一次上真机才暴露的。

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
| 5 | 隔离准备步骤化 + 逆序回滚 | **完成**。`PrepareSequence` |
| 6 | 取消/超时/后代清理统一停止流程 | **完成**。停因唯一入口 + 停止阶梯 + 有界预算 |
| 7 | 输出有界、语义清晰 | **完成**。EOF / 写入完成 / 落盘三件事分开 |
| 8 | workspace 与 runtime 边界 | **完成**。`keep_on_close`、容量/条目限制、`/input` 的 `snapshot_copy` 均已生效（默认仍是 `bind_readonly`，不再有兜底切换） |
| 9 | 持久化记录独立建模 | **完成**。`SessionStore` 接口只收 `AcceptedCommand` / `FinishedCommand`，落盘走 `core/record_json`；旧 v2 记录仍可读回；SDK 直读 `result.json` 时按记录格式映射 |
| 10 | 围绕故障边界的测试 | **完成**。截断/损坏/乱序/超时/取消/后代残留等 |

## 4. 剩余工作（按建议顺序）

每项都遵循同一条纪律：**改完 → 虚拟机跑全套 → 绿了才提交**。中途不提交、不改 D 盘。

### 4.1 `SessionStore` 迁移到类型化记录（**已完成**，不要再做一遍）

落盘形状（`record_schema_version = 3`，与 `schema_version = 2` 的会话快照文件是两套版本）：

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
- 验证边界要说清楚：process 模式本来就不允许 mounts，所以 `/input` 只有 isolated 模式才存在；本机 `apparmor_restrict_unprivileged_userns=1`，`sandbox doctor` 的探测卡在 `make_mounts_private`（EACCES，DEPLOYMENT.md 第 9 行记的同一件事），隔离路径从来没在这台机器上起来过。
- **已在此机器上验到的部分**（不依赖 namespace，因为复制发生在 launcher 之前、纯宿主侧）：用真实 config 跑 `sandbox session-open`，隔离如预期在启动阶段失败，但 run 目录里
  - `config.json` 的 `/input` 挂载源已被改写为 `<run>/input`；
  - `<run>/input/data` 与 `<run>/input/sub/deep` 内容与宿主一致（含子目录）；
  - 之后改宿主源文件，副本仍是原内容；
  - `keep_on_close=true` 时副本与 workspace 一起保留；`false` 时两者都被删除，且 `cleanup_errors` 为空。
- **仍未验到的部分**：把这份副本挂进沙箱 namespace（挂载代码本轮未改）。要覆盖它需要在允许非特权 userns 的机器上跑 `tools/delegated_run.py`（见 ACCEPTANCE.md §3），`sessions.py` 的 isolated 分支已有对应断言。

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
- **磁盘上有 12 个文件是 CRLF、索引里是 LF，而 `git status` 报干净**（索引 stat 缓存挡住了，`git ls-files --eol` 显示 `w/crlf`）。改动它们之后直接 `git add` 会把 CRLF 写进 blob，静默破坏上面那条 LF 纪律。名单：`include/sandbox/store.hpp`、`src/storage/run_store.cpp`、`src/runtime/session_protocol.cpp`、`src/application/service.cpp`、`src/platform/environment.cpp`、`sdk/sandbox_client.py`、`sdk/session_client.py`、`third_party/spdlog/{INSTALL,LICENSE,README.md,.gitignore,cmake/spdlogConfig.cmake.in}`。判别方法：`git hash-object <file>` 与 `git ls-files -s <file>` 的 blob 不一致而 `git status` 干净。提交前先 `-replace "\r\n","\n"` 归一，`git diff` 就只剩真实改动

### 存储

- **两套 `schema_version` 不要混**：会话快照 / artifacts 是 `2`（`SessionStore::write()` 负责盖章），命令记录是 `record_schema_version = 3`（由 `encode()` 自带）。记录文件必须走 `store_.write_json(name, encode(record))`，走过 `write()` 会被改成 2，然后被 `read()` 当成坏记录
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

绿基线比新功能重要。任何一步把 32/32 弄红的改动，都应该当场回退，而不是"再改改就好了"。
