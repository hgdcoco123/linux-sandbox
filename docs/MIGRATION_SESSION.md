# 会话路径迁移步骤（P3.6）

> 目标：让 `SessionController` 与 `SessionStore` 不再以 `Json` 为业务对象。
> 前置：`core/ops_json`（已完成）与 `core/ops`、`core/record`（已完成）。
> 本文是执行顺序与红线，不是设计讨论；设计见 [REDESIGN.md](REDESIGN.md)。

## 为什么不能一次做完

`interactive_session.cpp`、`session_store.cpp`、`command_model.*`、`session_protocol.*`、
`sdk/session_wire.py`、`tests/sessions.py` 之间存在双向依赖：控制器要 `SessionStore` 的
`replay/accept`，而这两者目前只接受旧 `ExecuteRequest`。一次改完等于同时换掉协议、存储、
SDK 和测试，中间态不可编译，也无法分阶段验收。

## 步骤

### 第 1 步：`SessionStore` 接受类型化请求（保留旧入口）—— **已完成**

落地方式与原计划的差异，写给下一个人：

- 没有保留旧重载。调用方只有三处，同一次提交里全部改完，旧 `accept(Json, Json)` / `finish(Json)` /
  `replay(ExecuteRequest&)` 直接删掉，避免两套入口长期并存。
- **旧记录仍然可读**：`session_store.cpp` 里有 `decode_accepted_record` / `decode_finished_record`，
  按 `index` / `command` 之类的键判别 v3 记录与 v2 旧形状，v2 只读不写。`session_store_tests`
  有一条测试专门写一份 v2 记录再读回来。
- 结果类型补了两个事实：`Cancelled::cause`（requested / session_closed）与
  `StartupFailed::exec_errno`。没有它们，旧回包里的 `stop_reason` 与 `exec_errno` 无法还原，
  而 `sessions.py` 断言了这两者。
- `AcceptedCommand` 增加 `effective_cwd`：会话会解析出实际工作目录，它属于命令事实但不属于请求意图
  （`same_intent()` 把 cwd 的存在性算进意图，折进去会让重放误报冲突）。

- 验收：`session_contract`、`session_store`、`sessions` 全绿；已跑 4 次稳定。

### 第 2 步：`SessionController::handle` 换成类型化 —— **已完成**
### 第 3 步：`CommandRecord` / `CommandFacts` 换成 `core` 类型 —— **运行时已完成，类型删除待做**

- 会话每条命令现在由 `ExecutionCore` 监督，结果直接是 `ExecutionResult`，`CommandRecord` / `CommandFacts` 已不参与运行时。
- 剩下的是把它们从 `command_model.*` 与 `task_tests.cpp` 里删掉（`ExecuteRequest` / `CommandId` / `WriteStdinRequest` 仍在外层边界使用，整文件删除还做不到）。
### 第 4 步：回包类型化 —— **已决定停在这里（见 REDESIGN §4.1）**

- 已完成：`SessionStore` 返回类型化 `CommandReport` / `SessionRecovery`，渲染搬到
  `session_wire::command_report_to_json()`。存储不再知道回包形状。
- 不再往下做：给 `SessionReply` 加 `RawReply{Json}` 把 status/files/活命令摘要包回去，只是给 JSON
  换名字、多一套会漂移的渲染器。协议（`proto/session_v1.proto`）本身就把 `metrics` 定义为不透明 blob，
  这个项目已经在根上接受了"观测数据不类型化"。

### 第 5 步：SDK 跟进（可选）

- `sdk/session_wire.py` 若要脱离 protobuf，可改用与 `core/wire.hpp` 相同的二进制帧；
  这是独立收益（去掉 Python protobuf 依赖），不是本重构的必要条件。

## 每一步的验收门槛

1. `tools/cross-verify.ps1` 全绿（含链接）
2. 该步涉及的 Linux 测试至少**编译 + 链接**通过
3. 不回退任何已有可移植断言

## 红线

- `same_intent` 语义、已落盘请求的可解析性
- 请求去重语义：接受后不得盲目重跑，终态不得被旧快照覆盖
- 16KiB 控制报文上限、FD 契约（`core/control` 已强制）
- 会话内一条命令的后代清空后才允许下一条（`persistent_init.cpp` 的
  `main_done && empty && observed` 三条件，不得放宽）
