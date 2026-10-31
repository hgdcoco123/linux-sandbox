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
| P3.5 | `RunSession`/`SessionController` 接入执行核心 | `[待做]` |
| P4 | 准备步骤 journal + 逆序回滚，产出 `CleanupResult` | `[待做]` |
| P5 | 合并 `init.cpp` 与 `persistent_init.cpp` 为一个 init | `[待做]` |
| P6 | 控制通道消息类型化，FD 契约按消息类型声明 | `[待做]` |
| P7 | 批次调度改用同一执行核心 | `[待做]` |
| P8 | 故障注入测试（步骤失败、FD 缺失/多余、写盘失败、恢复中断） | `[待做]` |

## 6. 验证方式

- 可移植部分：本机 `g++ -std=c++20`，`core_tests` 已通过。
- Linux 专有部分：zig 0.13.0 `-target x86_64-linux-gnu` 交叉编译；**交叉编译通过不等于内核行为通过**，仍须在真实 Linux 上跑。
