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

- `include/sandbox/model/command_model.hpp`（`RequestId`/`CommandId`/`CommandFacts`/`CommandRecord`）
- `include/sandbox/runtime/session_protocol.hpp`（`validate_session_request`、`SessionError`）
- `src/runtime/interactive_session.cpp`（`handle(Json)`、`start(ExecuteRequest)`）
- `src/storage/session_store.cpp`（`replay`/`accept`/`find_request` 的 JSON 语义）
- `sdk/session_client.py`、`sdk/session_wire.py`（客户端协议）
- `tests/integration/sessions.py`、`session_sdk_tests.py`、`session_contract_tests.cpp`

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

- **运行期 workspace 配额：已实现（tmpfs，不接受扫描方案）**。隔离会话的 `/output` 是
  `size=workspace.max_bytes` + `nr_inodes=workspace.max_entries` 的 tmpfs，配额**累计整个会话**、
  由内核强制，不存在按命令重置的路径，也没有任何抽样扫描。宿主看不到 namespace 里的 tmpfs，
  所以数据经 `/transfer` 双向移交：上传先暂存再由沙箱取走，读取（manifest/export/关闭时保留）
  先让沙箱交出整棵树再原子发布；移交失败会记进 `workspace.export` 与 `errors`，`keep_on_close`
  时不会报成功。四类用例（容量耗尽、inode 耗尽、导出失败、init 中途被杀）在
  `tests/acceptance/workspace_quota.py`，随 `tools/isolated-accept.sh` 一起跑，跳过即判失败。
- **runtime 布局新增 `transfer` 占位目录**：工作区挂载点在 runtime 里，因为 runtime 在装挂载前
  已被递归设为只读。旧 runtime 需要用 `tools/make_runtime.py` 重新生成。
- **P1 已修：传输通道 fd 化**。宿主传输目录不再挂进沙箱，而是随 `TransferChannel` 消息以
  **一个描述符** 交给 init，复制用 `export_workspace()`/`import_workspace()` 以 `*at` 系统
  调用完成（`publish_tree` 复用同一个 `walk()`，符号链接/硬链接/特殊文件的拒绝与边读边校验
  原样保留）。因此 `/transfer` 在沙箱文件系统里**不存在**，目标既不能写它绕过配额，也无法
  干扰暂存数据；`transfer-probe` 已从 `KNOWN GAP` 转为**硬断言**。
  另附传输时序：单在途 + 序号回显 + 超时作废通道（迟到回执按序号丢弃），
  超时上限可用 `SBX_TRANSFER_WAIT_MS` 调整；**`0` 表示"完全不给时间"**，用来让超时路径
  确定性地发生（非数字值仍回落到默认 30000）。
  **已端到端验证**：`bash tools/isolated-accept.sh` 全绿（七条成功行含
  `a timed-out transfer is reported and the channel is quarantined`），
  宿主传输目录在沙箱内不可达、发布为原子交换、超时后通道作废均已实测。- **导出阻塞控制通道：已定方案，待实现**。现在复制在 init 的控制循环里同步跑，超时只能作废
  通道、不能停复制。实现路径（不需要额外进程）：
  1. `walk()` 加一个 `const std::function<bool()> &interrupted`，每处理一个条目检查一次，
     为真时抛 `TransferAborted`（内部异常类型，与"复制失败"区分开）；
  2. `publish_tree` / `export_workspace` / `import_workspace` 透传该回调，签名与 `publish_tests`
     同步更新；
  3. init 的 transfer 处理器传入一个 lambda：`control.receive(true)` 非阻塞读一帧，若是 `Stop`
     或 `CancelCommand` 则记录"已请求停止"并返回真；处理器随后回 `Transferred{ok=false,
     error="interrupted by stop"}`，循环再按已记录的停止请求收尾；
  4. 监督端收到 `ok=false` + 该错误文本时记 `TRANSFER_INTERRUPTED`，**不作废通道**（复制确实停了，
     这是它与超时的本质区别）。
  验收：`publish_tests` 增加"复制到一半中止 → 抛 `TransferAborted` 且什么都没发布"。- **`core/wire.cpp` 的去留：保留，但要说清它是什么**。已核实：只有 `wire_tests` 与 `fault_tests`
  引用它，**运行期没有任何消费者**（真实的回包路径是 `session_wire` 的 Protobuf 编解码）。
  `REDESIGN.md` 把它写成"若要求内部也用 protobuf，替换点只有这一个文件"，所以它不是被遗忘的
  死代码，而是**被保留的设计替换点**。代价是那两组测试覆盖的是没人运行的代码——因此这里明确
  记录，避免有人把"wire 测试通过"当成"回包路径被覆盖"。
- **编译警告已清零（含测试）**：干净重编 `build-warn` 从 33 条降到 **0**。生产代码的两类
  （`decltype(&::closedir)` 的 `-Wignored-attributes`、4 处 `-Wmisleading-indentation`）已改代码；
  测试中间刻意忽略 `[[nodiscard]]` 的 28 条用**目标级** `-Wno-unused-result` 关掉。
  注意这条经验：编译选项必须加在**目标已定义之后**——之前把它放进文件顶部的列表里，
  静默无效，警告一条没少。
- **单次执行与批量任务的磁盘配额：已实现（决策 A）**。语义：隔离模式下 `/output` 一律是
  tmpfs（配额与会话一致），`config.mounts[target=/output].source` 的含义从"工作区本身"变为
  **最后一次导出落地的目录**；批量任务的子进程就是 `sandbox run`，因此自动继承（批次按子运行的
  `verdict` 判定，"产物未交付"会让任务失败）。实现落在 `run_session`（transfer 目录、`TransferChannel`
  fd、`Transferred` 与原子发布、等导出答案的预算）、`init::run_once`（收 fd、`export_workspace("final")`）、
  `rootfs`（有 transfer 时不 bind `/output`）与报告新增的 `workspace{export{attempted,ok,bytes,entries,error}}`
  （报告 schema 2）。用例：`tests/acceptance/isolation.py` 的单次配额与"导出被拒绝"两条，随
  `tools/isolated-accept.sh` 必检。**改动后必须重跑隔离验收**：本机 ctest 只能证明宿主侧逻辑。
  细节见 [HANDOFF](HANDOFF.md) §4.7 与 [HANDOFF_QUOTA](HANDOFF_QUOTA.md)。
- **会话准备已接 `PrepareSequence`**（本轮完成）：步骤 = state_root / control_socket / cgroup /
  input_snapshot / launcher，失败按逆序撤销；`errors` 里保留原始失败原因（`preparation failed
  at <step>: <reason> (undone: …)`），回滚失败单独进 `cleanup_errors`（`rollback <stage>: …`），
  `stop_reason` 用 `prepare_failed` 与运行期失败区分。`tests/integration/session_prepare.py` 注入两个失败点
  （末步 launcher、中间步 cgroup）验证这三件事。
- P7 批次接入执行核心（**已定为范围外**：批次属于多运行编排；做的话按"子运行落
  `FinishedCommand` 记录、批次解码记录而非匹配 `summary` 字符串"的方式）
- 隔离路径（`tools/delegated_run.py`）只在放开 `kernel.apparmor_restrict_unprivileged_userns`
  的机器上跑得起来；最后一次全绿是在 init 合并**之前**，合并后未重跑


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
