# 如何阅读当前完整工程

> 本文保留前版结构/原理。0.5 的类型模型、输出线程、事件循环、批次调度以 [TASK_ENGINE.md](TASK_ENGINE.md) 为准；当前学习路线见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。

> 0.4 核心结构更新以 [CORE_STRUCTURE.md](CORE_STRUCTURE.md) 为准；学习顺序见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。下文保留原有隔离原理与功能说明。

先看整体角色和正常执行，再看异常。第一轮只沿着一个 /usr/bin/sandbox-fixture echo 任务追踪。

1. `profiles/process-demo.json`：理解请求的数据。
2. `include/sandbox/config.hpp`、`report.hpp`：输入、输出、状态。
3. `apps/cli/main.cpp`：命令如何交给 SandboxService。
4. `src/application/service.cpp`：配置校验与执行入口。
5. `include/sandbox/session.hpp`：一次运行拥有哪些对象。
6. `RunSession::run / prepare / finalize`：先跳过 supervise 细节。
7. `UniqueFd` 与 `LauncherClient`：spawn 前怎么布置 FD，为什么先备份到100以上。
8. `apps/launcher/main.cpp`：P1 为什么存在，怎么 clone3、传 pidfd、wait init。
9. `src/runtime/init.cpp`：PREPARE/EXEC，fork目标、错误管道、MAIN_EXIT、后代回收。
10. `RunSession::on_launcher / on_init`：哪些消息推动状态变化。
11. `supervise` 与 `OutputCapture`：事件、限额、EOF 与取消。
12. `RootfsBuilder`、`SecurityPolicy`、`CgroupV2`：再学习每一种 Linux 隔离机制。
13. `RunStore / ControlServer`：查询、取消、持久化和保守恢复。
14. `tests/integration.py` 与 `isolation.py`：预测每个场景的状态和报告，再运行验证。

新增周期采样建议先读 `docs/METRICS.md`，再读 `MetricSample`、`MetricsSampler::sample()` 和 `RunSession::sample_metrics()`，最后看 `tests/metrics_tests.cpp`。这里的纯解析与计算测试可以在没有 Linux 内核的环境运行。

建议对每个类回答三个问题：它拥有哪个资源？谁能调用它？失败时由谁收尾？

不要先研究第三方 JSON 或 spdlog 源码。它们的作用是简化边界处理，项目的重点是 Linux 执行生命周期。

阅读中保持四种结果分离：目标退出事实、宿主停止请求、内核资源事件、清理结果。超时不等于目标一定由 SIGKILL 结束；输出 EOF 不等于后代全部退出；主进程退出也不等于可删除资源组。
