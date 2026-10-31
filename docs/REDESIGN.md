# 内部重构设计（0.6）

> 目标：把沙箱当作一个独立执行系统重新组织内部结构。**不承诺兼容 0.5 的协议与存储格式。**
> 状态标记：`[完成]` 已落地并有可移植测试；`[进行中]` 已定接口；`[待做]` 未开始。

## 1. 总原则

1. **业务层不出现 JSON。** JSON 只存在于三个边界：配置文件、CLI 输入输出、持久化记录。
2. **构造即合法。** 外部文本只经过一次 `parse()`；业务层拿到的类型不可能非法。
3. **事实分开表达。** 目标进程的结局、输出的去向、环境的回收，是三个独立事实，不合并成一个 `summary` 字符串。
4. **不知道就写不知道。** `Indeterminate` 是一等结果，绝不伪造退出码。
5. **每一步可回滚。** 隔离准备按步骤推进，任一步失败按逆序回收已完成的部分。
6. **每一层可编译、可测。** 不合并"先大改、后修编译"的提交。

## 2. 核心类型（`include/sandbox/core/`）`[完成]`

| 文件 | 内容 | 关键决定 |
|---|---|---|
| `ids.hpp` | `ExecutionId`、`RequestKey`、`CommandIndex` | 只能 `parse()`/`mint()`；`"cmd-01"` 这类多拼写直接拒绝 |
| `request.hpp` | `ExecutionRequest` | `create()` 一次性校验 argv/cwd/env/timeout；`env` 用 `optional` 表达"继承"与"显式清空"的区别 |
| `outcome.hpp` | `ExecutionOutcome` = `variant<Exited, Signaled, TimedOut, Cancelled, Oom, StartupFailed, SupervisionFailed, Indeterminate>` | 只有 `Exited` 有退出码；`Signaled` 永远不会被折成 128+n |
| `result.hpp` | `OutputResult` / `CleanupResult` / `ExecutionResult` / `Timing` | 见下节 |
| `record.hpp` | `AcceptedCommand` / `FinishedCommand` | 意图先落盘、终态后落盘，两者是不同记录 |
| `record_json.hpp` | 编解码边界 | 解码不抛异常；损坏的记录是数据，不是崩溃 |

`src/core/record_json.cpp` 是**唯一**同时包含 nlohmann/json 与业务类型的地方。

## 3. 结果三分 `[完成]`

```cpp
struct ExecutionResult {
    ExecutionOutcome outcome;   // 目标进程发生了什么
    OutputResult out, err;      // 捕获到了什么，丢了什么
    CleanupResult cleanup;      // 环境回收到了什么程度
    Timing timing;
};
```

- `outcome_exit_code()` 只看 outcome。目标退出 0 时**永远不会**因为清理失败被改写成 124/125。
- 清理失败通过 `cleanup.failures`（带 `CleanupStage`）与 `verdict_of()` 的 `cleanup_incomplete` 单独暴露。
- `OutputResult::complete()` 明确区分"截断"（主动丢弃）与"写失败"（丢数据）。
- 旧实现 `RunReport::conclude()` 把 `cleanup_errors` 并入 `host_error` 的路径在新模型里不存在。

## 3.5 控制报文：内部二进制，对外 protobuf

**决策**：`sandbox` ↔ `sandbox-launcher` / `init` 的内部通道不再用 JSON，也不用 protobuf，
而是 `core/wire.hpp` 定义的长度前缀二进制帧（magic `SBX2` + kind + tag + varint 长度）。

- 消息集合是封闭的（10 种请求、8 种回复），不需要 schema 演进。
- 旧实现的真正问题不是"JSON 慢"，而是"protobuf 解成 JSON、再解成业务对象"的三段式；
  换编码本身解决不了它，**改成类型化对象**才解决。
- 帧头在解析任何字段之前就拒绝：magic 错、kind 错、长度越界、payload 后面还有字节、超过 16KiB。
- 结果负载不重复定义：`ResultReply` 搬运的就是 `record_json.hpp` 产出的存储记录，控制层不解释它。
- 对外 SDK 通道（`proto/session_v1.proto`）仍是 protobuf，两条通道互不影响。
- 若要求内部也用 protobuf，替换点只有 `src/core/wire.cpp` 一个文件。

## 3.6 执行核心：事件进、动作出

`core/supervisor.hpp` 把单次执行与会话共用的监督逻辑抽成一个**不碰内核的确定性状态机**：

- 输入是事实：`env_ready`、`main_exited(code|signal)`、`descendants_empty`、`output_eof`、
  `output_finished(stats)`、`oom_observed`、`tick(now)`
- 输出是决策：`SignalTarget(SIGTERM)`、`KillTarget()`、`ForceDrain()`、`Completed()`
- 时间由调用方传入，所以测试不 sleep、不需要内核、不需要 root

三条规则被固化在这里，两个控制器不可能各写一套：

1. **事实优先于意图**：先退出 0 再收到 cancel，结果仍是 `Exited(0)`；cancel 之后才被 KILL，
   结果按 `TimedOut`/`Cancelled` 报，不会把自杀信号当成目标的信号。
2. **完成需要三个独立事实**：结局已定、后代清空、两条输出都已落盘。缺一个就不算完成；
   到 deadline 仍缺，就完成并把缺口写进 `cleanup.failures`（退出码不变）。
3. **准备阶段失败是立即完成**：什么都没启动，就没有输出可等、没有后代可收，
   `StartupFailed` 会明确写下这一点，而不是让调用方等一个永远不会到来的 EOF。

## 3.7 准备步骤与回滚

`core/prepare.hpp`：`PrepareSequence` 接收有序的 `{step, apply, undo}`，按序执行，
任一步抛异常就**按逆序撤销已完成的前缀**。

- 顺序是类型的一部分：`PrepareStep` 的枚举值就是依赖顺序，注册时乱序/重复/缺 action 直接
  `std::invalid_argument`——准备顺序写错是编程错误，不是运行时状况。
- **失败的那一步不撤销**：它没有完成，撤什么是它自己的责任；journal 不猜。
- **撤销失败不中断后续撤销**：每次都尝试，失败写进 `rollback_failures` 并带上所属
  `CleanupStage`，否则会把后面的资源留在系统里。
- 允许稀疏序列：process 后端没有 cgroup / id_map，序列里不出现这些步骤即可。
- `describe()` 给出一行可读结论，直接进日志与报告。

**接线（P4.5）**：`RunSession::prepare()` 把 control socket、cgroup、输出捕获、launcher 四步
交给 `PrepareSequence`，失败时逆序撤销并给出一行原因。状态目录**不撤销**——报告要写在那里；
launcher 步骤的撤销是 `LauncherClient::abort()`。

接线时发现一个只有真跑才会暴露的缺陷：执行核心在正常退出后进入 `draining` 而不是
`stopping`，而报告状态机只允许 `stopping → finalizing`，于是"正常跑完"会触发非法迁移。
现在只要核心离开 running 就把报告状态推进到 stopping，`finalize()` 入口再兜一次。

## 3.8 RunSession 已接入执行核心

`src/runtime/run_session.cpp` 不再自己判断超时、宽限期、强杀时机与完成条件：

- 事件：`ENV_READY` / `EXEC_OBSERVED` / `MAIN_EXIT` / `START_FAILED` / `INIT_ERROR` /
  通道意外关闭 / 后代清空 / 输出 EOF 与落盘完成 / 每个事件循环迭代的 `tick`
- 动作：`SignalTarget(SIGTERM)` → 向 init 发 `STOP`；`KillTarget` → `cgroup.kill_all()`
  （无 cgroup 时退回 init pidfd）；`ForceDrain` → 停止接收新输出；`Completed` → 退出循环
- 结束条件只剩一句 `if (core_->finished()) break;`

`RunReport` 现在是**适配层**：`summary`、`exit_code`、`term_signal`、计时、清理失败全部从
`ExecutionResult` 读出，`RunReport::conclude()` 不再参与判定。因此"目标退出 0 但清理失败"
不会再被改写成 `host_error`——它显示为 `success` + `cleanup_errors`，并可用
`verdict_of()` 得到 `cleanup_incomplete`。

## 3.9 控制通道：消息类型化 + FD 契约是数据

`core/control.hpp` 把 launcher/init 通道的消息也变成封闭变体（17 种），并规定每种消息
**必须携带几个描述符、用来做什么**：

| 消息 | FD | 用途 |
|---|---|---|
| `init_created` | 1 | init 的 pidfd |
| `stdin_handle` / `stdout_handle` / `stderr_handle` | 1 | 目标进程三条管道的对应端 |
| 其余 13 种 | 0 | — |

- `validate_fd_count(kind, arrived)` 在解释 payload **之前**就拒绝多余/缺失的描述符；
  旧实现是 `kind == "INIT_CREATED" || "STDIN" || ...` 的布尔表达式。
- `command` 携带的是执行配置的**不透明 blob**——配置是边界，控制层不解释它。
- 帧格式 `CTL1 + kind + varint 长度`，与会话通道共用 `core/binary.hpp` 的编解码原语，
  不再各写一份读写器。
- **契约两端都强制**：发送端拒绝"给 body-only 消息附带描述符""该带描述符却没带"；
  接收端在解包之后用 `validate_fd_count()` 再核对一次，多余描述符在抛错前已被 `UniqueFd` 接管，
  不会泄漏。
- launcher、init、persistent init、单次执行、连续会话五处全部改用类型化消息，
  `Message{Json body}` 这个结构已经不存在。

## 3.10 故障边界测试（P8）

`tests/fault_tests.cpp` 不测正常路径，只测"必须被拒绝"的输入：

| 边界 | 做法 |
|---|---|
| 报文截断 | 对合法报文的**每一个真前缀**逐一解码，必须全部抛错 |
| 报文位翻转 | 每一位分别翻 0x01 / 0x80，要么被拒绝，要么解码出同一消息类型 |
| 超大报文 | 超过 16KiB 在分配前拒绝 |
| FD 契约 | `command_done` 携带描述符必须被拒 |
| 事件乱序 | 重复 `main_exited` 不得移动计时锚点；完成后的事件不得改动结果 |
| 事实缺失 | 写线程完成但无 EOF 不得算完成 |
| 损坏记录 | 缺字段、类型错、未知 schema 版本 → 返回 nullopt + 原因，不抛异常 |
| 回滚失败 | 撤销抛异常时仍逆序继续，失败归到正确的 stage |

这一套当场发现两个真缺陷：

1. 重复的 `main_exited` 会把 `exit_at_` 挪到第二次上报的时刻，导致 `execution_ms` 偏大；
2. `outputs_finished()` 只检查"写线程结束"，没检查"管道 EOF"，于是**没有 EOF 也能算执行完成**——
   与设计中"EOF 和落盘是两个事实"直接矛盾。

两处都已修正，并各自留下断言。
## 4. 仍然沿用的决定
- 单次执行与会话共享执行语义，但**生命周期不同**：单次执行结束即回收环境，会话保留 init 与 workspace。
- 停止流程只有一条：记录原因 → 停止受理输入 → 请求退出 → 宽限期 → 强制终止 → 确认集合为空 → 收尾输出 → 落盘。取消与超时只是入口不同。
- 输出仍是有界队列 + 写线程；`write()` 完成不等于 `fsync()` 完成，报告里始终分开。
- 隔离准备失败一律停止启动，**没有**退回 process 模式的路径。

## 5. 路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1 | 核心类型、结果三分、记录边界 + 可移植测试 | `[完成]` |
| P2 | 类型化会话请求/响应 + 二进制控制报文（`core/ops.hpp`、`core/wire.hpp`） | `[完成]` 模型与编解码；控制器接线在 P3 |
| P3 | 统一执行核心 `core/supervisor`（事件→动作，纯逻辑） | `[完成]` 核心与测试；两个控制器的接线在 P3.5 |
| P3.5 | `RunSession` 接入执行核心（单次执行） | `[完成]` |
| P3.6 | `SessionController` 接入执行核心（连续会话） | `[待做]` |
| P4 | 准备步骤 journal + 逆序回滚（`core/prepare`） | `[完成]` |
| P4.5 | `RunSession` 用准备序列执行并在失败时逆序回滚 | `[完成]` |
| P5 | 合并 `init.cpp` 与 `persistent_init.cpp` 为一个 init | `[待做]` |
| P6 | 控制通道消息类型化 + FD 契约（`core/control`） | `[完成]` |
| P6.5 | `protocol.cpp`/launcher/init/会话/runsession 全部改用 `ControlMessage` | `[完成]` 控制通道已无 JSON |
| P7 | 批次调度改用同一执行核心 | `[待做]` |
| P8 | 故障边界测试（截断/位翻转/契约违规/乱序事件/损坏记录） | `[完成]` |

## 6. 验证方式

- 可移植部分：本机 `g++ -std=c++20`，`core_tests` 已通过。
- Linux 专有部分：zig 0.13.0 `-target x86_64-linux-gnu` 交叉编译；**交叉编译通过不等于内核行为通过**，仍须在真实 Linux 上跑。
