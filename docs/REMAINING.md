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

## 未完成

### P3.6 会话路径类型化（最大一块）

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

### 其它

- workspace/runtime 边界落成配置项（配额、保留策略、快照语义）尚未做。
- `RunReport` 只是适配层，最终应改为由 `ExecutionResult` 直接序列化。
- 会话路径的 `sessions.py` 等集成测试未更新，当前仓库状态下这些用例不可用。

## 真实性声明

交叉编译只能证明类型与接口正确。死锁、竞态、信号时序、cgroup 行为、seccomp 策略
都必须在 Ubuntu 上以真实内核验证。任何"完成"结论都不应基于本仓库当前的本机验证。
