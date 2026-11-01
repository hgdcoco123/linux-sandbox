# 交接文档：沙箱内部重构

写给下一个接手的会话。读完这份就该知道：改到哪了、怎么验证、还剩什么、哪些坑已经踩过。

## 1. 现在的状态

| 项 | 值 |
|---|---|
| 仓库 | `D:\cpp_project\linux-sandbox`（工作目录唯一权威） |
| 提交 | `ec5e441`，工作区干净 |
| 测试 | **32/32 通过**，Linux 7.0 内核实机，连跑 4 次稳定 |
| 跳过项 | `isolation_integration`（无 cgroup 委派时的预期行为，不是失败） |

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
| 1 | 业务层全类型化，JSON 只在配置/CLI/持久化适配层 | **部分**。请求分派已是 `std::visit(SessionRequest)`；`session_wire.cpp` 仍 Protobuf→JSON；`SessionStore` 仍存 JSON |
| 2 | 单次与会话共用一套执行核心 | **单次完成**（`ExecutionCore`）；**会话未接入**，仍是自己一套监督循环 |
| 3 | outcome / output / cleanup 三分 | **完成**。`verdict` 与 `summary` 分开，清理失败走 125 而 `summary` 仍可 success |
| 4 | 进程与 FD 所有权明确 | **完成**。17 种控制消息、`fd_contract`、收发两侧强制校验 |
| 5 | 隔离准备步骤化 + 逆序回滚 | **完成**。`PrepareSequence` |
| 6 | 取消/超时/后代清理统一停止流程 | **完成**。停因唯一入口 + 停止阶梯 + 有界预算 |
| 7 | 输出有界、语义清晰 | **完成**。EOF / 写入完成 / 落盘三件事分开 |
| 8 | workspace 与 runtime 边界 | **部分**。`keep_on_close` 已实现、容量与条目限制生效；**`/input` 仍是只读绑定，不是快照** |
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

### 4.2 `/input` 快照复制（下一步）

- 现状：输入目录以只读 bind 挂进沙箱，宿主后续修改会改变沙箱内的可见内容
- 设计要求：输入是**复制后的快照**
- 目标：把复制做成 `PrepareSequence` 的一个步骤，失败按逆序回滚；复制时沿用 `artifacts.cpp` 的目录 FD 语义（拒绝符号链接、特殊文件、路径替换）
- 相关：`core/workspace.hpp` 的 `WorkspacePolicy` 已有容量/条目/深度限制

### 4.2 `session_wire` 类型化（下一步）

- 目标文件：`src/runtime/session_wire.cpp`
- 现状：Protobuf → JSON → 业务层 → JSON → Protobuf，两层转换
- 目标：Protobuf → 业务类型 → Protobuf；JSON 只留给 CLI
- 会牵动：`sdk/` 下的 Python 客户端与 `tests/session_wire_*`、`tests/session_wire_interop`
- 建议保留 `RawReply` 之类的过渡类型给 `status`/`files`，先切执行路径再切其余

### 4.3 会话控制器接入执行核心（最后，影响面最大）

- 目标文件：`src/runtime/interactive_session.cpp`（约 560 行），重写 `event()` 与监督循环
- 现状：会话每条命令自己维护一套 `CommandExecution` + 监督循环，与 `RunSession` 是两份实现
- 目标：复用 `ExecutionCore`，会话只负责"命令之间保持环境"这一差异
- 直接受影响的测试：`sessions`、`session_runtime`、`session_store`、`session_sdk`
- 前提：先做完 4.1，否则存储层仍是旧模型，改完要改两遍

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
