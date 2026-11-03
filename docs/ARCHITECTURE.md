# 当前实现架构（与源码对应）

> **重构中（0.6）**：执行路径现由 `core/supervisor`（事件→动作）、`core/prepare`（步骤与回滚）
> 和 `core/control`（类型化控制消息）驱动；下文关于 `Message{Json}` 与
> `RunSession::request_stop` 式判定的描述属于历史实现。当前状态见 [REDESIGN.md](REDESIGN.md)
> 与 [REMAINING.md](REMAINING.md)。
>
> 本文保留前版结构/原理。0.5 的类型模型、输出线程、事件循环、批次调度以 [TASK_ENGINE.md](TASK_ENGINE.md) 为准；当前学习路线见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。

> 0.4 核心结构更新以 [CORE_STRUCTURE.md](CORE_STRUCTURE.md) 为准；学习顺序见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。下文保留原有隔离原理与功能说明。

> 文件环境顺序已修正：P2 在 pivot_root/卸载旧根之前，将属于新 PID namespace 的 procfs 挂到新根/proc。
> 不使用宿主 proc 的绑定；随后再切根、卸载旧根并关闭来源 FD。

> 0.3 新增 `InteractiveSession → persistent_init` 连续会话分支，下面内容主要描述原单次 `RunSession`。
> 两条路径共享 launcher、资源、输出、指标和存储模块。完整会话流程见 [PROJECT_GUIDE.md](PROJECT_GUIDE.md)。

## 1. 四个进程角色

```text
P0 sandbox CLI / RunSession
 │    配置、状态、日志、I/O、资源控制、报告
 └─ P1 sandbox-launcher
      │    clone3 创建环境、传出 init pidfd、wait 回收 init
      └─ P2 init（isolated 模式是 PID namespace 内 PID 1）
           │    环境准备、启动主目标、回收后代
           └─ P3 target
                └─ descendants
```

P0/P1 留在目标 cgroup 外，P2 归组后，P3 和后代自然继承归属。组级统计包含 init，不包含宿主控制进程和 launcher。

P0 通过 posix_spawn 启动全新的 P1。P1 是单线程进程，clone3 不共享 VM/FD 表；P2 在子分支进入 init_main。P0 只能 wait P1，P1 才是负责 wait P2 的父进程；pidfd 不能绕过这层父子关系。

## 2. 主要对象

| 对象 | 文件 | 持有什么 | 负责什么 |
|---|---|---|---|
| SandboxService | application/service.cpp | 状态根与 launcher 路径 | 组织命令用例，校验状态目录与映射分离 |
| RunConfig | model/config.cpp | 规范化配置 | JSON、单位、字段、路径校验 |
| StateMachine | model/report.cpp | 当前状态 | 拒绝非法迁移 |
| RunSession | runtime/run_session.cpp | 本次运行全部活动对象 | 准备、监督、停止、收尾 |
| LauncherClient | runtime/launcher_client.cpp | 子进程、pidfd、两条控制通道 | FD 布局、spawn、身份、回收兜底 |
| ControlChannel | runtime/protocol.cpp | socket 与收发序号 | 有界 SEQPACKET JSON 协议、SCM_RIGHTS |
| OutputCapture | runtime/output_capture.cpp | 管道读端、文件、计数 | 固定缓冲排空、捕获上限 |
| CgroupV2 | resources/cgroup.cpp | 叶子组路径及移除状态 | 配置、归组、强杀、采集、删除 |
| MetricRegistry | resources/metrics.cpp | 采集器对象 | 组合指标，局部失败不编造零 |
| MetricsSampler | resources/metrics.cpp | 每运行独立的采集器、前次计数和有界历史 | 周期调度、速率计算、汇总及实时快照 |
| RootfsBuilder | isolation/rootfs.cpp | 无常驻宿主状态 | 只在 P2 构建文件环境 |
| SecurityPolicy | isolation/security.cpp | 临时过滤器 | 权限收缩、目标信号、seccomp |
| RunStore | storage/run_store.cpp | 运行目录 FD、独占锁、owner | 状态快照、报告、inspect、恢复 |
| ControlServer | storage/run_store.cpp | UNIX 监听与有界待处理连接 | 核对同用户请求、异步接收 cancel |
| UniqueFd / Poller / SignalSource / TickSource | platform/linux.cpp | 内核资源 | 所有权和系统接口封装 |

RootfsBuilder 和 SecurityPolicy 没有强行持有不必要的状态。核心面向对象的价值在于资源与生命周期归属，而不是把每个系统调用变成虚函数。

## 3. 从用户到运行

```text
Cli main
  → SandboxService::run
    → RunConfig::load / validate
    → 验证状态目录不在暴露目录中
    → RunSession::run
       → prepare
       → supervise
       → finalize
    → RunReport::json / cli_code
```

`prepare()` 创建受保护目录和日志、控制 socket、资源组、管道、输出文件、根挂载点，最后 spawn launcher。

配置错误不创建目标。prepare 中途失败保留报告，进入显式清理。没有“隔离失败后直接在宿主重试”的分支。

## 4. 启动协议的精确顺序

1. P0 将配置序列化到已封印的 memfd，并打开运行时/映射源目录 FD。
2. P0 把所有来源 FD 先复制到 >=100，再通过 spawn 文件动作复制到固定布局，避免 dup2 覆盖另一个来源。
3. P1 读取 memfd、校验配置，clone3 创建 P2。P2 先等待控制消息。
4. P1 向 P0 发 INIT_CREATED，附宿主 PID 和 clone3 获得的 pidfd。
5. P0 写 P2 的 setgroups/uid_map/gid_map，把 P2 放入配置完成的叶子 cgroup。
6. P0 发 PREPARE；P2 才开始 rootfs 和权限准备。
7. P2 发 ENV_READY，P0 记录执行预算起点并发 EXEC。
8. P2 fork P3；P3 设置 cwd、信号、进程组、FD 白名单、seccomp，然后 execve。
9. P2 观察 exec 错误管道，发送 EXEC_OBSERVED 或 START_FAILED。
10. P2 对主目标 wait 取得事实，发送 MAIN_EXIT；回收后代后发送 INIT_DONE 并退出。
11. P1 wait P2，发送 INIT_REAPED。P0 wait P1，并独立检查组、输出及报告。

EXEC_OBSERVED 是 CLOEXEC 错误管道 EOF 的观察，不代表目标一定执行过某条用户指令；exec 前异常死亡的歧义通过退出事实和未知字段保留。

## 5. FD 布局

spawn 到 P1 的固定布局：

| FD | 内容 |
|---|---|
| 0 | 目标输入：普通文件或 /dev/null |
| 1 / 2 | 目标 stdout/stderr 写端 |
| 3 | P0 ↔ P1 的 launcher 控制通道 |
| 4 | 配置 memfd，读取后关闭 |
| 5 | P0 ↔ P2 的 init 控制通道 |
| 6 | 运行时目录 FD，isolated 模式 |
| 7+ | 映射来源目录 FD |

P1 clone 后关闭 0/1/2/5 和来源目录 FD，保留 3 及整理到 4 的 init pidfd。P2 关闭 launcher 通道 3，rootfs 完成后关闭来源目录 FD。P3 exec 前只保留 0/1/2 与 CLOEXEC 错误管道；exec 成功后只剩 0/1/2。

P0 不持有目标输出写端，P1 和 P2 也必须及时关闭它们，否则 EOF 永远不会到达。

## 6. 文件环境

- 可信静态运行时目录绑定到运行实例的 rootfs 挂载点。
- 对运行时视图施加递归只读、nodev、nosuid 等标志。
- 白名单 /input 与 /output 的目录映射都通过已打开目录 FD 绑定；/input 强制只读，映射不允许执行。
- 隔离模式的 /output 是例外：它是 namespace 内的 workspace tmpfs（`size=workspace.max_bytes`、
  `nr_inodes=workspace.max_entries`），宿主目录只是**导出目的地**，不是被挂进沙箱的工作区。单次执行与批量任务
  在目标结束后一次性导出并原子发布，会话按需移交；目标既看不到宿主目录，也没有 `/transfer` 路径（通道以 fd 交给 init）。
  只有 process 模式把 /output 直接绑到宿主目录（诊断后端，无配额）。
- /work 与 /tmp 各有大小受限 tmpfs；该内存也受组级限制约束。
- /dev 是小 tmpfs，仅绑定 null、zero、random、urandom。
- 在 P2 中先将新 PID namespace 的 procfs 挂到新根/proc，不绑定宿主 proc。
- 然后 pivot_root、切换 cwd、卸载旧根，关闭来源目录 FD。

运行时必须预先有占位目录。没有自动依赖收集，也不支持任意设备映射和任意目标挂载路径。配置提交者可信，来源父目录不得恶意并发替换。

## 7. 状态与事件

```text
Created → Preparing → Starting → Running → Stopping → Finalizing → Finished
               \          \          /           
                └─失败/取消─→ Stopping
```

RunSession 是状态写入的唯一所有者。Poller 返回事件，RunSession 做决策；输出采集器和指标采集器不直接改变生命周期。

epoll 处理两个输出 FD、两条内部控制通道、信号、20ms timerfd 和本地取消监听。每条输出每轮读取最多 64KiB，避免洪泛饿死其他事件。

控制消息会在结算前额外非阻塞排空：不能因为已经 wait 到 launcher，就假定 init 的 MAIN_EXIT 已被读取。不同 FD 的事件返回顺序不代表内核中的物理先后。

## 8. 超时和取消

准备/启动有 startup_ms；执行预算从发 EXEC 前计时，有 wall_ms；停止有 grace_ms。超时检查存在轮询和调度误差，最终 execution_ms 含终止/收尾开销，不是精确目标 CPU 时间。

取消入口为 SIGINT/SIGTERM 或同用户 UNIX socket。连接可能先建立、消息稍后才到，因此服务器保留最多 16 个、最多 500ms 的待处理客户端，不阻塞事件循环。

首次停止请求保存原因，发 STOP；init 温和通知后代并回收。宽限期到期后 isolated 后端写 cgroup.kill。process 诊断后端先让 subreaper 清理，额外留 1 秒后才考虑终止 init；它不具备 cgroup 级逃逸防护。

## 9. 收尾条件

正常结束需同时满足：

- launcher 已由 P0 回收。
- init 已由 P1 回收，且有 INIT_DONE 或控制通道关闭事实。
- cgroup 无任务；process 模式只能依据 init 已完整回收的报告。
- stdout/stderr 已 EOF。

达不到条件会在收尾期限后记清理不完整。finalize 再次尝试终止、等待、读指标、删空资源组和根挂载点，写最终报告；失败字段与目标退出码分开。

OOM 可能同时杀死 init 与目标，不能伪造目标退出码。只有组内 memory.events.local 的 oom_kill 事件支持时，才给出 oom 摘要；没有证据的异常控制进程退出作为宿主错误。

## 10. 状态目录与恢复

```text
/tmp/sbx-UID/run-24位随机十六进制/
  lock                活跃实例 flock
  owner.json          UID、boot_id、进程启动身份、cgroup 根
  config.json         实际配置
  state.json          阶段快照
  control.sock        同用户取消
  internal.log        控制日志
  stdout.bin
  stderr.bin
  result.json         完成报告
  recovery.json       遗留恢复记录（若执行 cleanup）
```

状态目录当前用户拥有、0700，文件0600。写 JSON 采用临时文件、fsync、rename、目录 fsync。输出目录由用户持有，恢复不删除输入、输出、报告。

cleanup 获取实例锁后才考虑操作，核对版本、ID 和用户。isolated 模式只处理记录对应的委派组；process 模式没有报告时无法证明逃逸后代归属，因此 unresolved，不按旧 PID 杀进程。

当前恢复不是跨内核资源的原子事务，也不保证 SIGKILL 后即时清理；支持边界见 LIMITATIONS。

## 11. 扩展方式

### 指标扩展

实现 `IMetricCollector::name/collect`，在 MetricRegistry 注册。已有 CPU、内存事件、PID 事件、内存峰值、当前内存、当前任务数和多设备 I/O 采集。每个 RunSession 持有一个 MetricsSampler，内部持有本次运行的注册表、计数基线与历史；CgroupV2 仅管理资源，不再临时构造采集器。采集失败写 null 和 unavailable 原因，不影响其余采集器，也不能决定目标是否结束。具体数据口径和配置见 METRICS.md。

### 执行策略扩展

先增加经过验证的 SecurityPolicy 策略和配置枚举，再在固定启动阶段调用。不能让模块任意改变“归组、文件环境、权限收缩、exec”的顺序。首版仅有 baseline 策略，不承诺动态 ABI。

### 上层集成

Python SDK/Agent → CLI 进程协议 → SandboxService → RunSession。SDK 不直接设置 namespace；当前没有复制一套轻量执行逻辑给 Agent。未来可新增 C++ SDK 适配入口，但仍 spawn 独立 launcher。

## 12. 与旧 M1 的实质区别

旧版主要是一段 execute 函数；本版按状态所有者与资源所有者分开，并具有独立执行进程、文件/资源策略、管理命令和外部调用适配。

本版没有为了“全部完成”伪造热插拔、分布式、生产安全或跨发行版保证。可编译性已检查，运行行为仍必须在 Linux 实验与 CI 中验证。
