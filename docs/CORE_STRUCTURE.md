# 核心结构整理：0.4

> 本文保留前版结构/原理。0.5 的类型模型、输出线程、事件循环、批次调度以 [TASK_ENGINE.md](TASK_ENGINE.md) 为准；当前学习路线见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。

本轮保留单次执行、连续会话、stdin、偏移输出、目录上传/清单/导出、指标和部署能力。
不增加完整工作区编辑 API、异步 SDK、线程池、多沙箱编排、远程服务或新的隔离后端。

## 模块和所有权

| 模块 | 文件 | 职责和所有权 |
| --- | --- | --- |
| SessionController | src/runtime/interactive_session.cpp | 监督循环、会话状态、操作调度；拥有 launcher、cgroup、服务端和活动命令 |
| CommandExecution | include/sandbox/command_execution.hpp | 一条命令的三个管道、两个输出捕获器、命令记录、采样基线和期限；准备完成才转移给控制器 |
| SessionLifecycle | include/sandbox/session_state.hpp | 合法状态转换、idle/running 操作前置条件 |
| WorkspaceService | src/storage/workspace_service.cpp | 会话工作区上传/清单/导出、受限清单响应；控制器检查 idle 和 isolated |
| SessionStore | src/storage/session_store.cpp | 会话快照、执行意图、命令终态、请求去重、恢复、关闭后查询 |
| RunStore | src/storage/run_store.cpp | 私有目录、进程锁、原子 JSON 写入、所有者证据、底层资源回收 |
| 会话协议 | include/sandbox/session_protocol.hpp、src/runtime/session_protocol.cpp | v1/v2 请求校验、ID、结构化错误、本地 socket 客户端 |
| 命令策略解析 | include/sandbox/session_policy.hpp | 在可信 RunConfig 的限制下解析命令请求 |
| 共享启动 | sdk/launch.py | CLI 包装器和 SDK 共用 systemd 委派参数、路径保护和启动方式选择 |

P0/P1/P2/P3 进程结构没有改变。内部 launcher/init 控制协议与外部会话协议版本分开。
所有权顺序：局部 CommandExecution 完整构造 → 执行意图落盘 → 转交控制器 → 发送 FD 和命令。
FD 发送后立即关闭 P0 保存的子进程管道端，避免自己的写端阻止输出 EOF。
准备失败释放 FD 和捕获器；已创建的命令目录可能保留作诊断，未接受请求不具有去重记录。

## 状态机

正常路径：preparing → idle → running → draining → idle。
运行中非终止状态可以进入 closing；关闭完成进入 closed；监督失败进入 failed，再执行清理并关闭。

| 操作 | 允许时机 |
| --- | --- |
| 新 execute | idle；同 ID、同参数的已接受请求先查询原命令，不创建新命令 |
| files | idle 且 isolated |
| stdin / stdin-close | running，并且命令 ID 仍匹配；stdin 必须未关闭才能写 |
| cancel | 活动命令 running；无活动命令的旧式无 ID cancel 保持无操作应答 |
| close | 幂等进入 closing；随后拒绝新执行和文件修改 |
| status / result / output / request-status | 服务存活时只读；SDK 保留已关闭结果与输出读取；request-status 另支持离线查询 |

COMMAND_DONE 表示 init 已完成目标和后代回收；监督器仍须等待 stdout/stderr EOF。
两者均满足才发布结果并回到 idle。closing 状态下收到 ENV_READY 或 COMMAND_DONE 不会重新开放会话。
closed 表示监督生命周期结束，是否有回收错误仍要检查 errors；它不是无条件的隔离清理成功证明。

事件入口保持集中：控制请求 handle、内部事实 event/drain_channels、输出 drain、期限检查。
本轮没有改成 epoll/线程池，同步文件 I/O 仍可能延迟控制响应。

## 协议 v2

仍使用同 UID 的 UNIX SOCK_SEQPACKET，每请求/响应不超过 16KiB，每连接处理一个请求。
请求顶层增加 protocol_version=2。execute 必须带 request_id，限制为 1..64 个 ASCII 字母、数字、连字符、下划线。
run_id 就是会话 ID；command_id 格式仍为 cmd-N。ID 不构成身份认证或多租户授权。

```json
{"protocol_version":2,"op":"execute","request_id":"task-17-build-1","argv":["/usr/bin/cc","main.c","-o","main"],"cwd":"/output","wall_ms":10000}
```

成功响应保留 ok/data；错误保留旧 error 文本，同时增加 error_detail：

```json
{"protocol_version":2,"ok":false,"error":"...","error_detail":{"code":"REQUEST_CONFLICT","message":"...","retryable":false}}
```

主要错误码：INVALID_REQUEST、UNSUPPORTED_VERSION、UNKNOWN_OPERATION、SESSION_NOT_IDLE、SESSION_CLOSING、
COMMAND_NOT_ACTIVE、COMMAND_LIMIT、POLICY_DENIED、REQUEST_CONFLICT、REQUEST_NOT_FOUND、STORAGE_FAILURE、OPERATION_FAILED。
CLI 会话错误在 stderr 输出结构化 JSON；SDK SandboxError 暴露 code 和 retryable。
传输失败可能仍是操作异常，不代表服务端未接受。retryable 只是提示，不是自动重试许可。

兼容：未指定版本按 v1；v1 execute 不带 request_id 时保留旧行为，**不承诺去重**。
v2 SDK 显式传版本且自动生成 ID。需跨进程重试的业务必须自行保存稳定 request_id；execute 抛异常时也附带 error.request_id。
不向旧服务端静默降级；升级时 CLI、SDK 和会话监督进程一起更新，不接管旧的活动会话。

## 执行意图与恢复

1. 验证请求/策略，准备命令资源。
2. cmd-N/request.json 持久化完整请求与命令身份，fsync 文件、命令目录及会话目录。
3. 发送 STDIN/STDOUT/STDERR/COMMAND，再更新 session.json。
4. 目标和后代退出、输出 EOF 后，先写 cmd-N/result.json，再更新会话快照。

同 ID、相同请求返回同一 command_id；不同内容返回 REQUEST_CONFLICT。比较的是去除协议版本后的请求 JSON，
显式写默认值和省略默认值仍算不同内容。去重范围是一份会话记录，记录保留多久，去重证据就保留多久。
查找最多扫描当前会话的 1024 个命令目录。本轮没有加入数据库或跨会话去重。

响应丢失后查询：

```json
{"protocol_version":2,"op":"request-status","request_id":"task-17-build-1"}
```

会话活跃时返回活动命令或终态；监督器已退出时 CLI 读取持久化意图和结果。
有接受记录但无活动事实/终态时返回 uncertain、outcome_known=false，不重新执行。
未找到意图返回 REQUEST_NOT_FOUND；若最初请求还在传输中，此结果不能证明它永远不会被接受，应使用同一个 ID 协调重试。

恢复扫描全部 request.json，覆盖“意图已落盘但会话快照仍 idle”的崩溃窗口。
已有匹配终态优先；未完成的意图写 interrupted、outcome_known=false。重复恢复保持终态不变。
已有结果损坏、命令身份不匹配或不支持的格式版本时拒绝覆盖，报告失败。
这提供避免盲目重执行的语义，不提供任意外部副作用的 exactly-once 承诺，也不恢复运行中的进程。

SessionStore 新写记录版本 2，读取旧记录缺省版本/版本 1；owner.json、state.json 的原有格式继续为 1。
不支持旧程序读取新会话并继续运行；回退程序前保留新版本记录。
结果文件持久化不代表输出文件具备完整掉电持久性，文件系统和硬件断电场景尚未验收。
process 模式崩溃时若无法证明后代归属，cleanup 继续返回 unresolved，不按猜测 PID 杀进程。

## 启动和策略

正式 CLI 入口 sandbox-run 与 SDK 共用 sdk/launch.py 的 delegation_command。
SDK create/start 的 startup 默认为 auto：process 需 allow_unsafe；isolated 已提供 cgroup_root 则复用外部委派，
否则进入 systemd 用户委派。可显式指定 direct/delegated，direct 不自动提权且需自行满足隔离条件。
SDK create 强制 --session，避免配置缺少 session.enabled 时误启动单次执行。
启动等待失败时停止明确命名的 systemd 单元，不只是杀死 systemd-run 客户端。
低层 sandbox run/session-open 继续适用于已有委派的调用者；不会递归调用包装器。

可信宿主提供 RunConfig（运行时、挂载、网络、cgroup、资源上限）；execute 只接受 argv/cwd/env/wall_ms。
wall_ms 只能在宿主上限内缩小，不能通过命令请求改变挂载或隔离。
这不是管理员策略服务器：同 UID 的可信调用者仍可提供自己的启动配置，宿主文件上传/导出路径仍由可信应用约束。
Agent 应保存宿主策略，将模型生成的参数放到命令请求里，不能把整份启动配置交给模型修改。

## 验收

本轮新增 session_contract、session_store、launch_contract，扩展 sessions 的丢响应去重测试。
Windows 可运行 portable 合约及 Python 测试；Linux 目标交叉编译不等于 Linux 运行测试。
真实验收命令与证据见 VALIDATION.md。上一版 Linux 集成成功不自动覆盖本次重构。
