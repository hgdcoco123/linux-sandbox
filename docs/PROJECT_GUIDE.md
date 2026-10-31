# Linux 程序沙箱完整项目说明（0.3 扩展版）

> 本文保留前版结构/原理。0.5 的类型模型、输出线程、事件循环、批次调度以 [TASK_ENGINE.md](TASK_ENGINE.md) 为准；当前学习路线见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。

> 0.4 核心结构更新以 [CORE_STRUCTURE.md](CORE_STRUCTURE.md) 为准；学习顺序见 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。下文保留原有隔离原理与功能说明。

> procfs 在 P2 中、新根的 proc 目录提前挂载，再切根并卸载旧根，以保留挂载时所需的已有 procfs 可见性。

> 已新增固定路径、应用专属授权和普通用户运行入口。Ubuntu 正式部署先看 [INSTALLATION.md](INSTALLATION.md)。
> 开发目录运行不自动获得已安装启动器的授权；探测通过也不替代完整隔离测试。

整理日期：2026-10-23。本文件覆盖整个项目的定位、已有实现、本次扩展、使用方式、设计契约和验收标准。
精确消息字段见 SESSION_API.md；原单次执行指标口径见 METRICS.md。

## 1. 项目解决什么问题

调用方提交程序及配置，沙箱负责构建受限环境、启动程序、限制资源、处理输入输出、终止后代、生成报告。
0.3 增加连续会话：环境只准备一次，在其中顺序执行多条独立命令，保留文件，最终导出产物并关闭。
真人可以通过 CLI 使用，Agent 和其他程序可以通过 Python SDK 使用。

这是 Linux 单机系统软件。当前无多沙箱调度、远程服务、账号系统、PTY 或 C++ SDK。
单次 CLI 监督进程和会话 CLI 监督进程都独立运行；没有新增统一常驻守护服务。
会话监督进程本身持续存活，客户端只是向其发送本地请求。

当前源码已实现相关路径，但新增 Linux 行为尚未在此 Windows 环境运行验收。
交叉编译和链接只验证构建，不能验证隔离、生命周期和恢复行为。

## 2. 两种执行方式

| 方式 | 生命周期 | 适用任务 |
|---|---|---|
| run | 一次准备、一次执行、结束回收 | 独立脚本、单次计算 |
| session-open | 一次准备、多次顺序执行、显式关闭或到期回收 | 编译测试、交互式决策、数据处理 |

两者仍有 isolated / process 两种后端。默认 isolated，缺少必需能力就失败。
process 必须明确 --allow-unsafe，仅用于可信程序调试；它不能提供沙箱隔离保证。

## 3. 原有能力

- C++20 / CMake；随源码提供 nlohmann/json 和 spdlog，Linux 依赖 libseccomp。
- 配置解析、未知字段拒绝、路径和数值校验。
- user / mount / PID / IPC / UTS namespace，可使用独立 network namespace。
- 只读 runtime、pivot_root、最小设备、独立 proc、受限 tmpfs。
- no_new_privs、capabilities 收缩、seccomp baseline。
- cgroup v2 CPU、内存、swap、pids 限制和组级清理。
- stdout/stderr 捕获上限，超限后仍排空管道。
- 启动时限、执行时限、取消、终止宽限和强制回收。
- 原子保存状态和结果，实例锁、资源归属记录、保守 cleanup。
- 每次运行独立采样 CPU、内存、任务数及块 I/O，有界历史、实时 inspect。
- doctor / validate / run / inspect / cancel / cleanup，以及原 Python SDK。

## 4. 本次增加的能力

1. 持久隔离 init，顺序接收多条命令。
2. 独立命令 ID、退出状态、启动错误、输出和结果文件。
3. 会话总时限、空闲时限、命令时限；取消命令和关闭会话分别处理。
4. 命令 stdin 字节流、显式 EOF、按偏移读取 stdout/stderr。
5. 会话专属 /output 工作区，允许执行其中编译生成的程序。
6. 目录上传复制、目录清单、完整工作区导出；SHA-256 内容摘要。
7. 有界复制、禁止链接及特殊文件、无覆盖发布、复制中修改检查。
8. Python SessionClient / SessionHandle / CommandHandle。
9. 会话异常退出后的中断记录、归属核验和 cleanup 恢复路径。
10. 可移植配置/摘要测试，以及待 Linux 执行的会话和文件集成测试。

## 5. 进程架构

```text
CLI 请求 / Python SDK
        │ 同 UID、本地 SOCK_SEQPACKET
        ▼
P0 会话监督进程（InteractiveSession）
 └─ P1 独立 sandbox-launcher
     └─ P2 持久 init（isolated 下为 namespace PID 1）
         ├─ P3 命令 A → 回收全部后代 → 报告
         └─ P4 命令 B → 回收全部后代 → 报告
```

P0/P1 不在目标 cgroup 内；P2 及命令树在同一个会话 cgroup 内。
命令严格串行，第二条命令必须等上一条的后代清理及 stdout/stderr EOF 后才允许执行。
P0 使用原 LauncherClient、CgroupV2、MetricsSampler、OutputCapture 和 RunStore。
原 RunSession 继续负责单次执行；InteractiveSession 负责新增会话路径。

新增核心文件：

| 文件 | 职责 |
|---|---|
| src/runtime/interactive_session.cpp | 会话监督、控制请求、预算、命令记录与收尾 |
| src/runtime/persistent_init.cpp | 环境准备一次、重复 fork/exec、等待和清理后代 |
| src/storage/artifacts.cpp | 基于目录 FD 的遍历、复制、清单和导出 |
| include/sandbox/sha256.hpp | 流式 SHA-256，供产物身份核验 |
| sdk/session_client.py | 会话客户端，复用 C++ 执行能力 |
| tests/session_model_tests.cpp | 会话配置边界、摘要标准向量 |
| tests/sessions.py | process 会话、文件及显式委派环境下的 isolated 冒烟 |

## 6. 启动与命令协议

启动沿用原握手：launcher 创建 init → P0 写 UID/GID 映射并归组 → PREPARE → ENV_READY。
到 ENV_READY 后 init 不退出，也不自动执行配置 argv。
配置 argv 保留用于结构校验；实际命令由 execute 请求指定。

每条命令：

```text
P0 创建三组管道与命令输出文件
 → STDIN（传 FD）
 → STDOUT（传 FD）
 → STDERR（传 FD）
 → COMMAND（配置和 command_id）
 → init fork / exec
 → exec 错误管道观察
 → 回收主进程与后代
 → COMMAND_DONE
 → P0 确认输出 EOF
 → 保存命令结果，恢复 idle
```

三个 FD 分帧通过 SCM_RIGHTS 传递，沿用内部协议版本与序号校验。
子进程 exec 前只保留标准输入输出与 CLOEXEC 错误管道。
命令执行失败和退出码 127 分开：exec_errno 非空表示启动路径报错。

## 7. 状态与取消语义

会话状态：preparing → idle ⇄ running → closing → closed。
命令状态：running → finished；监督异常中断时为 interrupted。
finished 代表命令执行事实收集完毕，成功与否需看 exit_code、signal、exec_errno、stop_reason。

| 操作或事件 | 处理 |
|---|---|
| 非零退出 / exec 失败 | 保存该命令结果，正常收尾后可执行下一条 |
| cancel | 停止当前命令，正常收尾后保留会话 |
| close / 总时限 / 空闲到期 | 停止当前命令并关闭整个环境 |
| OOM 导致 init 死亡 | 关闭异常会话；未知目标退出状态不伪造 |
| 客户端断连 | 不等同于取消，监督进程继续到显式关闭或期限 |
| 监督进程 SIGKILL | 不保证即时完成回收，后续 cleanup 核验归属并处理 |

同一会话最多执行 1024 条命令，避免命令记录无限增长。
会话总时限包含准备与空闲。空闲时限从环境就绪或上一条命令完成后计算，查询状态不续期。
文件操作耗时包含在空闲时间中；传输是同步的，期限检查可能延迟。
单条命令 wall_ms 必须为正且不超过配置 limits.wall_ms。

## 8. 文件和 Shell 状态

保留的是环境与文件，不保持某个 Shell 的变量或当前目录。
每条命令独立传 argv、cwd、env；默认 isolated cwd 为 /output。
env 如显式传入则替换本条命令的环境映射，不与上一条命令合并。
命令结束后清理其后代；第一版不支持有意保留后台服务。
PTY 和持久 Shell 都不在本次范围。

isolated 会话的 /output 来自实例 workspace 目录，只有该目录暴露给目标。
运行记录、控制 socket、owner 和结果文件所在的父目录不挂载给目标。
/work 和 /tmp 的 tmpfs 也随环境存活，但当前文件交付 API 只管理 /output。
process 模式不提供受管理的隔离工作区；可用可信宿主 cwd 测试连续执行，文件 RPC 被拒绝。

## 9. 文件交付契约

上传是“宿主目录 → /output/新目录名”，不覆盖已有目录。
导出是“整个 /output → 一个尚不存在的宿主目录”。
离线 files 命令也可以对任意可信本地目录生成清单或复制目录。
要读取或修改工作区内文件，可在会话内执行相应程序；当前没有专门的文件编辑 RPC。

规则：

- 只接受普通文件和目录，拒绝符号链接、多个硬链接和设备、FIFO、socket 等特殊文件。
- 遍历路径组件使用目录 FD 与 O_NOFOLLOW，避免跟随链接。
- 每次操作最多 4096 个条目、累计读取 256MiB。
- 内容摘要为 SHA-256；复制后文件权限为 0600，保留源文件的属主执行位时为 0700。
- 不复制 setuid/setgid、扩展属性、ACL、时间戳及其他用户权限。
- 文件复制检查前后身份、大小和修改信息；失败不发布为成功目标。
- 先写同一父目录下的 .sbx-copy-*，完成后通过 NOREPLACE rename 发布。
- 目标存在时拒绝，避免静默覆盖；失败暂存目录保留，需人工核实后处理。
- 完整清单保存在 artifacts.json，控制响应最多返回前 24 项，并标记截断。

这里的输入快照是独立复制，完成后不会随原文件修改而改变。
它不是文件系统级原子快照；源目录不得被不可信进程并发修改，跨文件的时间点一致性不保证。
传输限额不等于运行期磁盘配额，程序仍可能在执行时写满输出所在文件系统。
当前未实现新增/修改/删除差异报告、单文件选择导出及可配置复制上限。
导出成功表示复制完整，不表示程序成功或产物可信；应同时检查命令结果。

## 10. 输出和背压

stdin RPC 使用字节数组，写入非阻塞管道，返回实际接受数量。
调用方负责对未接受部分重试；0 表示当前背压，不能当作 EOF。
stdin-close 才是 EOF。命令结束后输入自动关闭。
建议每块最多 2048 字节，保证十进制 JSON 编码后落在 16KiB 协议上限内。

stdout/stderr 独立保存，按字节偏移读取，每次最多 2048 字节。
两个流没有全局严格顺序。达到 capture_bytes 后丢弃后续内容，但继续排空管道。
实时读取通过有界轮询实现，没有增加推送订阅线程或无界内存队列。
取消和关闭 RPC 的 accepted 仅表示请求接收，必须继续观察状态确认完成。

## 11. 指标口径

会话级指标覆盖 init 和目标树，包含准备、空闲及全部命令；历史按 metrics.history_limit 有界保留。
命令结果提供该命令期间 cgroup CPU 累计计数的差值，以及观察到的最大采样内存。
它们仍包含 init 开销及会话共享内存，不是仅目标程序自身的统计。
命令内存采样峰值可能漏掉短峰值；会话内核 memory.peak 不能直接当成每条命令的峰值。
process 模式不使用整机指标代替单会话消耗。
当前命令级报告未提供独立 I/O 汇总和独立采样历史，完整 I/O 和历史位于会话结果。

## 12. 持久化与恢复

```text
run-ID/
  owner.json / lock / config.json / state.json
  session.json              会话实时状态
  control.sock              同 UID 控制入口
  workspace/                映射到 /output
  cmd-1/
    stdout.bin / stderr.bin / result.json
  cmd-2/...
  artifacts.json            最近一次文件操作完整清单
  result.json               会话最终结果
  recovery.json             cleanup 的恢复记录
```

状态/结果沿用临时文件、fsync、rename 的保存方式。
cleanup 先取得实例锁，校验归属后处理隔离 cgroup，不根据旧 PID 盲目杀进程。
异常会话在归属资源处理成功后写 closed / recovered_after_interruption；未完成命令写 interrupted，退出码未知。
process 模式缺少完整后代归属证据时保留 unresolved，不伪装恢复成功。
恢复用于回收与说明中断，不接管原任务继续执行。
不自动删除工作区和历史产物，尚无保留期和全局磁盘治理。

## 13. 构建与基本运行

```bash
sudo apt-get install build-essential cmake python3 libseccomp-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Windows 只能运行可移植模型、指标与摘要测试，不能执行 Linux 沙箱。
快速测试可信 process 会话：

```bash
# 终端一，保持前台运行
./build/sandbox session-open --config profiles/session-process-demo.json \
  --state-root /tmp/sbx-session-demo --allow-unsafe
# 终端二，从 /tmp/sbx-session-demo/run-*/session.json 取得 run_id
```

真实 isolated 运行前必须准备 runtime 和 cgroup 委派。
静态 fixture runtime 只有测试程序，不包含 Shell、Python 或编译器。

```bash
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output /tmp/sandbox-runtime
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --config "$PWD/profiles/session-isolated-demo.json" \
  --state-root /tmp/sbx-session-demo
```

wrapper 根据 session.enabled 选择 session-open，并生成正确的委派 cgroup_root。
SDK 示例使用 process 模式演示 API；isolated 时换成准备完整的真实配置：

```python
from sdk.session_client import SessionClient

client = SessionClient("./build/sandbox", "/tmp/sbx-client")
with client.create("profiles/session-process-demo.json", allow_unsafe=True) as session:
    command = session.execute(["/bin/sh", "-c", "printf hello"])
    result = command.wait(timeout=5)
    print(result)
    offset = 0
    while True:
        chunk = command.output(offset=offset)
        print(chunk["bytes"])
        offset = chunk["next_offset"]
        if chunk["finished"] and not chunk["bytes"]:
            break
```

isolated 会话上传和导出：session.upload("./source", "src")；session.export("./new-results")。
编译命令需要 runtime 自备编译器与库，并指定 cwd="/output/src"。
SDK wait 超时仅停止等待；cancel 停止命令；close 销毁会话。
上下文管理器退出时 close；仅丢弃 Python 对象不自动销毁会话。
重连使用 client.attach(原 state_root, run_id)。SDK 创建的每个会话使用独立 state_root。

## 14. 如何验收项目

1. 先执行模型、指标、配置与摘要测试。
2. Linux 执行 process 集成与 sessions 测试：连续命令、失败后复用、取消后复用、二进制输入输出、限额输出。
3. 在明确委派环境运行 isolated 测试，包括会话上传、连续执行和产物导出。
4. 进一步人工/自动验收：会话到期、空闲到期、主进程和后代被终止、OOM、监督进程 SIGKILL、cleanup 重复执行。
5. 配备编译器 runtime 后，演示上传源码 → 编译 → 执行 → 导出；当前 fixture 不能替代这个场景。

委派测试入口：

```bash
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --test-runtime /tmp/sandbox-runtime
```

该入口依次运行原 isolation 测试及新增 sessions 测试。
实际验证状态见 VALIDATION.md；没有运行的测试不得记作通过。

## 15. 审阅时重点看哪里

- 是否真实保留同一个 namespace/init，而不是每次重建环境。
- 命令完成是否同时等待后代清理和输出 EOF。
- 非零退出与环境故障是否区分；取消是否误关会话。
- 文件操作是否能够跟随目标制造的链接访问宿主。
- 控制协议、输出、复制、命令数和采样历史是否有明确上限。
- 监督进程死亡后是否保留未知事实，而不是编造成功退出。
- 会话指标是否被错误表述为单条命令或单目标指标。

## 16. 明确的当前限制与下一步

本版不宣称生产安全，不提供多租户边界，配置调用者仍为可信本地用户，共享宿主内核。
同步输出写盘与文件传输仍可能延迟监督循环；没有严格实时响应保证。
文件交付失败的暂存目录和累计输出还需要空间治理；没有完整磁盘配额。
会话没有独立命令 cgroup，因此不能在杀死 init 后仍保留原环境。
OOM、内核异常和清理失败可能导致整个会话不可用，这是明确终止路径。
没有动态权限策略、后台服务保留、持久 Shell、PTY、远程执行和多沙箱调度。

下一步应先在真实 Linux 修复集成测试暴露的问题，再增加目录差异、精细导出、异步文件处理和更全面故障注入。
单会话验收稳定后，再独立设计多实例资源预算、排队和管理服务。
