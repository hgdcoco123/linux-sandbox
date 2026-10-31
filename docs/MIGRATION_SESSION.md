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

### 第 1 步：`SessionStore` 接受类型化请求（保留旧入口）

- 在 `session_store.hpp` 增加 `replay(const ExecutionRequest&, ...)` 与
  `accept(const ExecutionRequest&, const CommandIndex&)` 重载，内部用
  `core/record` 的 `AcceptedCommand` 落盘，`same_intent()` 由 `ExecutionRequest` 提供。
- 旧重载保留并委托到新实现（把旧 `ExecuteRequest` 转成 `ExecutionRequest`），
  这样仓库始终可编译。
- **红线**：`same_intent` 的判等语义必须保持——`cwd`/`env` 的"存在性"参与比较，
  显式空 env 与继承 env 不是同一个请求。`core_tests` 已有对应断言。
- 验收：`session_contract`、`session_store` 测试仍通过；新增一条"旧记录可被新代码读回"的用例。

### 第 2 步：`SessionController::handle` 换成类型化

- 签名改为 `Json handle(const SessionRequest&)`（回包暂时仍是 JSON，见第 4 步）。
- 用 `std::visit` 分派，逐个 op 从 `op.xxx` 取值，不再出现 `j.at("op")`。
- 入口处调用 `session_request_from_json()`；解码失败直接返回错误回包，
  不再把 `Json` 传进业务层。
- 需要旧类型的地方（`resolve_command`、`CommandRecord`）先就地转换一次，
  并在注释里标 `// transitional`。
- 验收：全仓库编译 + 链接；`sessions.py` 未改动仍能过（它只依赖回包格式）。

### 第 3 步：`CommandRecord` / `CommandFacts` 换成 `core` 类型

- `CommandRecord` → `core/record` 的 `FinishedCommand` + `core/result` 的 `ExecutionResult`。
- `CommandFacts::parse(Json)` 删除；事实来自 `core/control` 的 `CommandDone`（已完成）。
- `command_model.*`、`session_protocol.*` 中不再被引用的声明删除。
- 验收：`session_contract`、`task_tests` 改写后通过；`command_model.*` 可整文件删除。

### 第 4 步：回包类型化

- 给 `SessionReply` 增加 `RawReply{Json}`，仅用于 `status`/`files` 这两个只读视图，
  并在文档里说明它是边界例外。
- 其余回包用已有类型（`Accepted`、`OutputChunk`、`ResultReply`…），
  由 `session_reply_to_json()` 编码回原格式，保证 SDK 不变。
- 验收：`tests/session_sdk_tests.py`、`tests/sessions.py` 不改仍通过。

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
