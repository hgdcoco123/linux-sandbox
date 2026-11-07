# Linux 程序沙箱

## 0.6 内部重构（进行中）

按"把沙箱当作独立执行系统重新组织内部结构"的目标，旧的 JSON 转发层已基本替换：

- `include/sandbox/core/` 是一层不依赖 JSON 的业务内核：类型化 ID 与请求、
  结果三分（outcome / output / cleanup）、执行核心（事件进、动作出）、准备步骤与逆序回滚、
  控制消息与 FD 契约、记录与其编解码、workspace 边界。
- 控制通道（launcher/init）已完全类型化，不再有 `Message{Json}`；两种 PID 1 已合并成一份。
- 单次执行与会话**都已接到同一个执行核心**上（会话每条命令一个 `ExecutionCore`）。
- 会话持久化记录已类型化；请求在入口处一次性解成业务类型。
- **仍是 JSON 的地方**（有意为之，见 [REDESIGN §4.1](docs/REDESIGN.md)）：协议回包、
  配置文件、CLI 输出、持久化记录的编码。
- **尚未完成**：批次仍是独立进程 + JSON 汇总；
  seccomp 仍是兼容型拒绝策略（只记录了名称与版本）。见
  [剩余工作](docs/REMAINING.md)。

**先读 [重构设计](docs/REDESIGN.md)、[剩余工作与验证边界](docs/REMAINING.md)，实机验收按 [验收手册](docs/ACCEPTANCE.md)；会话路径迁移的执行顺序见 [会话迁移步骤](docs/MIGRATION_SESSION.md)。**
本机可复现的验证（可移植测试 + 交叉编译 + 链接）见 `tools/cross-verify.ps1`。

**验证状态（0.6）：** 在 Ubuntu 24.04 上跑过全量隔离验收——`ctest` 33/33、
`tools/isolated-accept.sh` 必检行全齐、服务 `status=0`（见 [ACCEPTANCE](docs/ACCEPTANCE.md) 顶部）。
交叉编译与链接仍只证明类型、接口与符号自洽；**其他发行版/内核、崩溃恢复与长期压力没有验证**。
## 0.5 本机任务执行引擎

新增类型化命令模型、有界输出写入队列与工作线程、共享 epoll 等待入口，以及 FIFO/资源预算限制的独立任务批次。
单次执行和连续会话继续保留。当前版本以 [TASK_ENGINE.md](docs/TASK_ENGINE.md) 为架构说明，
[LEARNING_ROUTE.md](docs/LEARNING_ROUTE.md) 为学习入口，[PERFORMANCE.md](docs/PERFORMANCE.md) 说明实验方法与限制。
**（历史，写于 0.5 轮）** 当时完成的是本地可移植测试与 Linux 交叉编译，真实运行验收留到 0.6 一并做（见上方验证状态）。

诊断批次示例：`./build/sandbox batch --config profiles/batch-process-demo.json --allow-unsafe`。
正式隔离批次使用 `/opt/linux-sandbox/bin/sandbox-run --batch --config FILE`，Python 使用 `sdk.batch_client.BatchClient`。


## 0.4 核心结构整理

保留现有功能，整理命令资源所有权、显式会话状态机、协议 v2 与持久化请求去重、统一恢复规则和 CLI/SDK 启动。
先看 [本轮架构与接口约定](docs/CORE_STRUCTURE.md)、[学习路线](docs/LEARNING_ROUTE.md) 和 [验收记录与命令](docs/VALIDATION.md)。
本轮本地可移植测试与 Linux 交叉编译通过，重构后的真实 Linux 运行待验收。


## 受管理的普通用户部署

新增 `tools/deploy.py`：提供安装计划、暂存打包、固定路径安装、完整性检查和卸载。
安装到 `/opt/linux-sandbox`，管理员一次性批准并加载只匹配已安装启动器的 AppArmor 兼容策略；
不设置 setuid、不关闭全局安全策略、不安装 root 常驻服务。
普通用户通过 `/opt/linux-sandbox/bin/sandbox-run` 申请用户 systemd 的 cgroup 委派后运行。
先看 [完整部署说明](docs/INSTALLATION.md)，并执行 `python3 tools/deploy.py plan --build-dir build` 审阅实际文件和权限。
上一版已通过真实 Ubuntu 安装和隔离执行验收；真实卸载仍未验收，本轮代码变更需重新跑 Linux 测试。

运行前先执行 `./build/sandbox doctor`，查看新增的 `isolation_probe`。
探测使用独立启动器验证基础 user/mount namespace 操作，不修改系统策略；具体部署边界见 [DEPLOYMENT.md](docs/DEPLOYMENT.md)。
探测通过仍需完整隔离测试；策略阻塞不会自动提权或退回无隔离模式。

## 0.3 连续会话与文件交付扩展（2026-10-23）

新增 `session-open`、`session-request` 和 `files`：同一隔离环境内顺序执行多条命令，
支持命令取消、会话关闭、期限回收、字节流 stdin、按偏移读取输出、目录上传和产物导出。
提供 Python `sdk.session_client.SessionClient`，暂不提供 C++ SDK 或多沙箱调度。

**完整项目文档：[PROJECT_GUIDE.md](docs/PROJECT_GUIDE.md)。**
**CLI、请求 JSON 与 Python 接口：[SESSION_API.md](docs/SESSION_API.md)。**

0.3 功能已有用户提供的真实 Linux 通过记录，0.4 重构需重新验收；详见 [VALIDATION.md](docs/VALIDATION.md)。
下面原有 `run` 使用方式仍保留。隔离模式下 `/output` 一律是沙箱自己的 tmpfs（单次执行、批次任务与会话同一条磁盘边界），
`mounts[target=/output].source` 是运行结束时最后一次导出落地的宿主目录；运行期间宿主目录看不到新产物。

一款 C++20 Linux 单机程序隔离执行软件。提供独立 CLI、JSON 配置、运行报告及 Python 进程式 SDK。宿主程序负责执行生命周期，独立 launcher 创建隔离 init，目标运行在 init 管理的环境中。

本版是可继续扩展的完整首版源码，不是已审计的生产安全产品。交付机器为 Windows；验证状态见 [docs/VALIDATION.md](docs/VALIDATION.md)。**已经完成 Linux 交叉编译与链接，不等于已经完成真实 Linux 隔离验收。**

## 代码入口

| 想了解什么 | 从哪里读 |
|---|---|
| 用户入口及命令 | `apps/cli/main.cpp` |
| 应用用例 | `src/application/service.cpp` |
| 一次运行的完整流程 | `src/runtime/run_session.cpp` |
| 进程、FD 和握手 | `src/runtime/launcher_client.cpp`、`apps/launcher/main.cpp` |
| PID namespace 的 init | `src/runtime/init.cpp` |
| 文件和权限隔离 | `src/isolation/` |
| 资源控制与扩展 | `src/resources/` |
| 查询、取消、持久化恢复 | `src/storage/run_store.cpp` |

详细关系见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，按任务阅读见 [docs/READING.md](docs/READING.md)。

## 已实现的完整主线

- JSON 配置解析、未知字段拒绝、数值/路径/映射校验及有效配置输出。
- 显式 launcher 子进程，clone3 创建 user/mount/PID/IPC/UTS namespace；禁网模式另建 network namespace。
- UID/GID 映射、启动握手，资源归组后才允许准备与执行目标。
- 只读运行时、目录映射、独立 tmpfs、pivot_root、新 procfs、最小设备环境。
- 能力收缩、no_new_privs、基于 libseccomp 的兼容型基础拒绝策略。
- cgroup v2 内存/swap/CPU/pids 限制、OOM 事件与指标采集。
- 每运行独立的周期采样、CPU/磁盘 I/O 速率、当前内存/任务数、实时 inspect 和有界历史。
- stdout/stderr 原始字节捕获、上限截断和持续排空。
- 墙钟超时、信号取消、本地控制 socket 取消、后代回收与整体强杀。
- 状态快照、JSON 报告、实例锁、受保护记录和保守遗留清理。
- `doctor` / `validate` / `run` / `inspect` / `cancel` / `cleanup`。
- 可注册的指标采集器、示例配置、静态测试运行时制作工具及 Python SDK。

## Linux 构建

建议首个运行环境：Ubuntu 24.04 x86_64、cgroup v2、允许所需 user namespace、提供所需委派。不是根据系统版本推断全部能力可用，运行失败会直接报告。

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake python3 libseccomp-dev protobuf-compiler libprotobuf-dev python3-protobuf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

json 3.11.3 与 spdlog 1.14.1 已随源码提供。libseccomp 和 Protobuf C++ 使用 Linux 系统开发包；Python 会话 SDK 需要 Python Protobuf（系统包或 `requirements.txt`）。CMake 用系统 `protoc` 从 `proto/session_v1.proto` 生成 C++ 类型；源码中的 Python 类型由同一文件生成。需要更新协议时执行 `protoc -I proto --python_out=sdk proto/session_v1.proto`。编译器和 C++ 运行库应来自同一发行版软件包；监督进程的动态库安装在宿主机，不放入目标沙箱运行时。`sandbox` 和 `sandbox-launcher` 必须在同一目录，或显式通过 `--launcher` 指定可信绝对路径。

Windows 仅支持配置/状态/报告模型测试，CMake 不会假装构建 Linux 运行时。

## 先运行诊断后端，理解执行架构

```bash
./build/sandbox doctor
./build/sandbox validate --config profiles/process-demo.json --allow-unsafe
./build/sandbox run --config profiles/process-demo.json --allow-unsafe
```

**process 模式只用于可信程序和开发测试，没有文件、网络或资源隔离。** 它复用 launcher、init、I/O、取消和报告流程，但不会把它伪装成安全沙箱。默认模式是 isolated；缺少隔离条件时绝不自动切换成 process。

运行结束 stdout 是 JSON 报告，日志到 stderr 和实例的 internal.log。报告里有 run_id。默认状态根目录 `/tmp/sbx-<uid>`，可用 `--state-root` 修改，路径要足够短以容纳 UNIX socket。

```bash
./build/sandbox inspect run-替换为实际24位十六进制ID
./build/sandbox cancel run-替换为实际24位十六进制ID
./build/sandbox cleanup
```

cancel 返回 accepted 仅代表请求接收；inspect 或等待 run 结束后才确认停止。Ctrl+C/SIGTERM 也会请求取消。

## 准备真正的隔离运行

1. 制作可信静态 fixture 运行时：

```bash
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output /tmp/sandbox-runtime
mkdir -p /tmp/sandbox-input /tmp/sandbox-output
printf 'sample\n' > /tmp/sandbox-input/sample.txt
```

2. 复制 `profiles/isolated-demo.json`，把 runtime_root、输入输出目录改成上述绝对路径。这个示例运行静态 fixture，**不能因此推断它已经具备 Python 的运行时依赖**。使用 Python 时需要另外准备解释器、库和必要文件。

3. 在提供 Delegate=yes 的 systemd 服务中执行。以下 wrapper 将控制进程放入 supervisor 子组、运行放入 runs 子树，从而满足迁移进程时的委派权限和父组无进程约束：

```bash
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --config /绝对路径/job.json
```

用户服务可能缺少控制器或 user namespace 权限。wrapper 会拒绝，不会提权重试。管理员可在专用虚拟机运行独立的 root systemd 服务做验证，但这不等于普通用户模式已验证。

已有委派环境的使用者可以直接在配置中设置正确的 `cgroup_root`：该目录必须是空的分配节点，已启用 cpu/memory/pids，且执行者具有移动进程所需共同祖先权限。仅 chown 一个任意 cgroup 目录通常不够。

## 验证隔离行为

普通 `ctest` 运行模型测试和 process 后端测试。隔离测试在没有显式环境参数时返回 77，CTest 显示 skipped，不会当成已验证。

```bash
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --test-runtime /tmp/sandbox-runtime
```

附带 GitHub Actions Linux 工作流，执行构建、普通测试与委派隔离测试。本次没有上传仓库，也没有实际运行远端 CI。

## Agent 等程序的调用

```python
from sdk.sandbox_client import SandboxClient

client = SandboxClient("./build/sandbox", "/tmp/my-sandbox-client")
handle = client.start("/absolute/path/job.json")
print(handle.inspect())
# handle.cancel()
report = handle.wait(timeout=10)
print(report)
print(handle.output_path("stdout"))
```

SDK 通过独立监督进程管理生命周期，会话控制直接使用本地 socket；隔离逻辑仍由 C++ 实现。调用方应自行限制 Agent 可使用的运行时、挂载和配置，不把可信本地配置接口直接暴露为多租户 API。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 目标成功并完成收尾 |
| 2 | 配置、命令或无法生成结果的入口错误 |
| 10 / 11 | 目标非零退出 / 信号终止 |
| 12 | 启动失败或启动超时 |
| 13 | 有本组 OOM 事件支持的资源超限结果 |
| 124 / 130 | 墙钟超时 / 用户取消 |
| 125 | 宿主错误、清理不完整或无法确认目标结果（`verdict` 非 ok） |

详细限制、恢复边界和统计口径见 [docs/LIMITATIONS.md](docs/LIMITATIONS.md)。

## 周期采样

isolated 模式可配置 `"metrics": {"interval_ms": 250, "history_limit": 256}`。
`inspect` 的 `state.metrics.latest` 提供最近样本，最终报告的 `metrics.sampling` 保存汇总和有界历史。
CPU 按一个逻辑核为 100% 计算；缺失或首次无法计算的速率为 null，不填假零。内核内存峰值与采样观察到的峰值分别记录。
process 诊断模式不读取整机数据冒充本次资源消耗。详见 [docs/METRICS.md](docs/METRICS.md)。


## 通用 AI 执行分析层（0.1）

新增独立的 `sandbox_ai` Python 包：执行证据（含 `internal.log` 生命周期日志）、结构化分析、
两阶段检索（BM25 / 向量 → RRF → rerank）、有界工具调用（含按区间重读输出流）、多轮上下文、
每条分析的 `metrics` 与 `stats` 聚合。

- **引用是行地址**：证据以 `{"id","lines"}` 供给，模型只回 `{"id","line"}`，原文由程序解析
  （`resolved_citations`）——模型没有伪造引用文本的通道。
- **假设状态闭合**：可选的 `hypothesis_updates`（supported / refuted / unknown）必须引用验证运行
  的证据，并记入库；`stats` 汇总。
- 受控验证通过现有 SDK 调用隔离沙箱，模板支持 `${run_dir}` 引用本次运行的产物；回执标注
  "已执行未自动证实"。不修改执行核心和原始报告。

- 一条命令看失败：`python -m sandbox_ai --provider compatible explain --run-dir <run>`
- 契约测试：`ctest --test-dir build -R ai_contracts`（或 `python -m unittest discover -s tests/unit/ai`，79 项）
- 端到端自检：`bash tools/ai-check.sh`（契约 + 离线 demo + 指标断言）
- 验证闭环演练：`bash tools/ai-verify-e2e.sh`（由操作者本人运行；真实隔离执行验证模板）
- 踩过的坑与修法（面试可讲）：[docs/RETROSPECTIVE.md](docs/RETROSPECTIVE.md)
- 价值证据（解释是否让下一次尝试更容易成功）：[docs/VALUE.md](docs/VALUE.md)
- 安装与启动模式：[docs/INSTALL.md](docs/INSTALL.md) · 契约与版本：[docs/CONTRACT.md](docs/CONTRACT.md) · 威胁模型：[docs/THREAT_MODEL.md](docs/THREAT_MODEL.md)
- 从 [使用与验证边界](docs/AI_ANALYSIS.md) 开始，模块说明见 [sandbox_ai](sandbox_ai/README.md)，
  已执行/未执行的验证记录见 [AI_VALIDATION.md](docs/AI_VALIDATION.md)。
离线演示及测试不等于真实模型或 Linux 隔离验收。
