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

### P3.6 会话路径类型化（最大一块）

**已完成的第一步**：`core/ops_json` 提供 `session_request_from_json()`，把客户端 JSON 请求
转成类型化 `SessionRequest`，带闭合字段集（未知字段拒绝）与逐字段类型检查；9 种操作 +
3 种 workspace 子操作全部覆盖，`ops_json_tests` 有 30 余条接受/拒绝断言。
下一步是把 `interactive_session::handle(const Json&)` 换成 `handle(const SessionRequest&)`,
这一步才需要动 `SessionStore` 与旧 `ExecuteRequest`。

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

### P5 合并两个 init

`src/runtime/init.cpp` 与 `persistent_init.cpp` 是两份 PID 1：reap 顺序、subreaper、
seccomp 安装点各写一遍。合并方式是给一个 init 加 `mode`（one-shot / persistent），
共享 prepare、exec、wait、reap 四段，只有命令循环不同。

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

- `src/storage/artifacts.cpp` 的复制循环改用 `admit()`，替换写死的 4096 项 / 256MiB
- 关闭时按 `keep_on_close` 决定保留还是删除 workspace，删除失败进 `CleanupResult`
- `/input` 的 `snapshot_copy` 模式尚未实现（当前仍是只读绑定）
- `RunReport` 只是适配层，最终应改为由 `ExecutionResult` 直接序列化。
- 会话路径的 `sessions.py` 等集成测试未更新，当前仓库状态下这些用例不可用。

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
