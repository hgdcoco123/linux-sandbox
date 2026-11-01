# 重构剩余工作

> 状态：核心层已完成并在单次执行路径上接线；会话路径与批次仍在使用旧模型。
> 所有 Linux 专有代码只经过交叉编译，**没有在真实内核上运行过**。

## 已完成

| 内容 | 文件 | 验证 |
|---|---|---|
| 类型化 ID / 请求 / 结果三分 / 记录边界 | `core/ids,request,outcome,result,record,record_json` | `core_tests` |
| 会话操作类型化 + 二进制会话编解码 | `core/ops,wire,binary` | `wire_tests` |
| 执行核心（事件进、动作出） | `core/supervisor` | `supervisor_tests` |
| 准备步骤 + 逆序回滚 | `core/prepare` | `prepare_tests` |
| 控制消息类型化 + FD 契约 | `core/control` | `control_tests`,`control_fd_tests` |
| 故障边界矩阵 | `tests/fault_tests` | 截断/位翻转/乱序/损坏记录 |
| 单次执行接线（核心 + 准备序列 + 类型化控制通道） | `runtime/run_session`,`protocol`,`init`,`apps/launcher` | 39/39 交叉编译 |
| 报告不再把清理失败并入 host_error | `model/report` | `model_tests` |
| 报告新增 `verdict`，退出码跟随 verdict（清理不完整 → 125） | `model/report`、`runtime/run_session` | `model_tests` |
| 批次任务按子运行的 `verdict` 判定，不再只看 `summary` | `runtime/batch` | 交叉编译 + 链接 |
| OOM 在报告层保留优先级（仅对"结局不明/被信号"的运行） | `runtime/run_session` | 交叉编译 + 链接 |
| 强制收尾时计时兜底为墙钟，不再报 0ms | `runtime/run_session` | 交叉编译 + 链接 |

## 未完成

### P3.6 会话路径类型化（**已完成**）

- `core/ops_json` 的请求解码、`interactive_session::handle(const SessionRequest&)` 的 `std::visit` 分派
- `SessionStore` 的持久化记录类型化（v3 记录格式，v2 旧记录只读兼容）
- `session_wire` 的请求方向：Protobuf 直接解成 `SessionRequest`，控制器只收一个处理函数
- **会话控制器接入 `ExecutionCore`**：每条命令一个 core，结果直接是 `ExecutionResult`；
  动作映射的差异（init 拥有进程组、`KillTarget` 升级为关闭会话）记在 HANDOFF §4.4

`CommandRecord` / `CommandFacts` / `StopReason` / `CommandPhase` 已从 `command_model.*` 删除，
`task_tests.cpp` 只保留 ID/请求解析与调度器断言。

**执行顺序与红线见 [MIGRATION_SESSION.md](MIGRATION_SESSION.md)。**

原始范围：

`interactive_session.cpp` 仍以 `Json` 为业务对象。要完成它必须同时替换旧模型，因为
`resolve_command`、`SessionStore::replay`、`CommandRecord` 都绑定在
`ExecuteRequest`/`CommandId` 上：

- `include/sandbox/command_model.hpp`（`RequestId`/`CommandId`/`CommandFacts`/`CommandRecord`）
- `include/sandbox/session_protocol.hpp`（`validate_session_request`、`SessionError`）
- `src/runtime/interactive_session.cpp`（`handle(Json)`、`start(ExecuteRequest)`）
- `src/storage/session_store.cpp`（`replay`/`accept`/`find_request` 的 JSON 语义）
- `sdk/session_client.py`、`sdk/session_wire.py`（客户端协议）
- `tests/sessions.py`、`session_sdk_tests.py`、`session_contract_tests.cpp`

建议顺序：先把 `SessionStore` 迁到 `core/record` 的类型化记录（请求/终态），再换
`handle`，最后改 SDK。迁移的红线是 `same_intent()` 的判等语义与已落盘请求的可解析性。

### P5 合并两个 init（**已完成**）

`init.cpp` 与 `persistent_init.cpp` 现在是**一份** `src/runtime/init.cpp`（`persistent_init.cpp` 已删除）：

- `init_main()` 只按 `config.persistent` 分派到 `run_once()` / `run_persistent()`。
- 共用的一段：`arm_init` / `prepare_environment` / PREPARE-EnvReady 握手 / `spawn_target()`（fork + 执行前设置 + exec-error 管道）/ `supervise_command()`（poll、waitpid、宽限期后升级 SIGKILL、完成条件）/ `signal_children()`（隔离模式 `kill(-1)`，诊断模式按 `/proc/.../children` 枚举）。
- 两种模式的区别被压缩成参数：`SupervisionRules`（wall 期限归谁、grace、关闭期限）与 `CommandWatcher`（是否**即时**上报 `MainExit`/`StartFailed`——单次执行需要，会话模式按 `CommandDone` 一次性上报）。
- exec 失败记录统一成 `ExecFailure{stage, error}`（写在 `init_common.hpp`），两种模式从此只有一个写者、一个读者。实测单次执行报 `exec_error: stage=6, errno=2`。
- **顺序上的红线**（合并时踩到过）：会话关闭时 init 必须先发 `CommandDone` 再发 `InitDone`，否则监督端永远等不到命令终态。实测关闭运行中的命令，记录里仍是 `cancelled/session_closed`。

### P7 批次接入

`batch.cpp` 仍用 `posix_spawn` 起独立 sandbox 进程并按 JSON 汇总状态。接入点是让每个
任务复用 `ExecutionCore` 的结果类型，批次摘要直接来自 `ExecutionResult`。

### workspace 边界接线

`core/workspace` 已给出策略类型与 `admit()` 决策函数（预算、结构拒绝、优先级、溢出安全），
并有 `workspace_tests` 覆盖。**尚未接线**：

已完成：

- `RunConfig` 的 `workspace` 段可配置并被校验（未知字段、未知 input 模式、零预算都拒绝），
  `to_json` 回写，`model_tests` 覆盖往返与拒绝路径

仍待做：

- **运行期 workspace 配额（未实现，真实缺口）**：`admit()` 与 `WorkspacePolicy` 管的是**复制**
  的预算；目标程序运行期间往 `/output`（会话 workspace 的绑定）写文件**没有任何强制限制**。
  `/work`、`/tmp` 是 `size=limits.tmp_bytes` 的 tmpfs，由内核强制；`/output` 是宿主目录绑定，
  大小与文件数都不受约束。两种可行做法见下面"运行期配额的两条路"。
- **seccomp 的 runtime 专用策略（未实现）**：当前是 `SCMP_ACT_ALLOW` + 拒绝列表的兼容型策略，
  结果里只记录了策略名与规则集版本。针对明确支持的 runtime 做经测试的 allowlist 是独立工程
  （需要对该 runtime 的 syscall 集合做 profiling 并用隔离环境验收），不是重构能顺带完成的。
- **会话准备已接 `PrepareSequence`**（本轮完成）：步骤 = state_root / control_socket / cgroup /
  input_snapshot / launcher，失败按逆序撤销；`errors` 里保留原始失败原因（`preparation failed
  at <step>: <reason> (undone: …)`），回滚失败单独进 `cleanup_errors`（`rollback <stage>: …`），
  `stop_reason` 用 `prepare_failed` 与运行期失败区分。`tests/session_prepare.py` 注入两个失败点
  （末步 launcher、中间步 cgroup）验证这三件事。
- P7 批次接入执行核心（**已定为范围外**：批次属于多运行编排；做的话按"子运行落
  `FinishedCommand` 记录、批次解码记录而非匹配 `summary` 字符串"的方式）
- 隔离路径（`tools/delegated_run.py`）只在放开 `kernel.apparmor_restrict_unprivileged_userns`
  的机器上跑得起来；最后一次全绿是在 init 合并**之前**，合并后未重跑

### 运行期配额的两条路

| 方案 | 内容 | 代价 |
|---|---|---|
| A. tmpfs + 收尾导出 | `/output` 挂 `size=workspace.max_bytes,nr_inodes=max_entries` 的 tmpfs，由内核强制；命令/会话结束时由 init 把树拷回宿主绑定（复用同一套"拒绝符号链接/特殊文件/边读边校验"的复制语义） | 复制语义要在 in-namespace 侧可用；init 多一个收尾步骤；`keep_on_close`/导出仍以宿主副本为准 |
| B. 宿主侧扫描兜底 | 监督循环里周期性统计 workspace 的字节与条目数，超预算即停止命令并把超限记进 `cleanup` 失败 | 不是硬配额：快速写入者可在两次采样之间超出（100ms 采样 ≈ 上百 MB 的过冲），且扫描有成本 |

建议 A：只有它将"运行期配额"变成内核强制的事实，而不是报告里的近似值；B 可以作为 A 落地前的临时措施，但结论必须写明"这是过冲有界的近似，不是配额"。

已决定不做（理由见 REDESIGN §4.1，不要重新论证）：

- 回包 payload 类型化（`RawReply{Json}`）
- per-command `metrics` 进记录
- `RunReport` 改成由 `ExecutionResult` 直接序列化

已完成：

- 复制循环改用 `admit()`：容量/条目/深度/结构拒绝的判定只在 `core/workspace` 一处，
  拒绝原因直接进错误文本（`entry_limit` / `byte_limit` / `symlink` / `hardlink` / `not_regular`）
- 关闭时按 `keep_on_close` 决定保留还是删除 workspace，删除失败追加进 `cleanup_errors`
- `/input` 的 `snapshot_copy`：复制在 launcher 前完成，见 `snapshot_inputs()`

'## 本机可复现的验证

```powershell
powershell -ExecutionPolicy Bypass -File tools/cross-verify.ps1 `
    -Zig <zig.exe> -HostWork <含 sysroot 与 protobuf 预编译对象的目录>
```

四段：可移植测试 → 全部 Linux 翻译单元与测试源码编译 → 链接 `sandbox`、`sandbox-launcher`
→ 链接全部测试可执行文件。数量随文件增减变化，脚本不写死。
第三步是关键：逐文件编译通过仍可能留下未定义或重复符号，链接才证明整棵树自洽。

## 实机验收

按 [ACCEPTANCE.md](ACCEPTANCE.md) 执行；里面同时列出了"最可能出问题的地方"和失败时要回传的材料。

## 交接要点（给下一个人）

1. 先读 `docs/MIGRATION_SESSION.md`（会话路径迁移的顺序与红线），再动代码
2. 先跑 `tools/cross-verify.ps1` 确认起点是绿的；它证明不了运行时行为
3. 报告字段有三个层次：`summary`（目标）、`verdict`（环境可否信任）、`exit`（调用方是否继续）；
   任何"改了一个字段含义"的改动，都要把所有读取方（CLI、批次、SDK、测试）逐一核对一遍——
   这条在 38 轮里触发过 4 次真实缺陷
4. 修改报告/事件的"适配层"时注意语句顺序：覆盖写在赋值之前等于没写（同一类问题出现过一次）

## 真实性声明'

交叉编译只能证明类型与接口正确。死锁、竞态、信号时序、cgroup 行为、seccomp 策略
都必须在 Ubuntu 上以真实内核验证。任何"完成"结论都不应基于本仓库当前的本机验证。
