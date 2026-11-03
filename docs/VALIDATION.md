# 验证记录

## JSON + Protobuf 改造（2026-10-30）

本机 Windows 验证：C++ 协议转换测试通过；C++/Python 双向互通 4 项、Python 协议与直连模拟 5 项、会话 SDK 11 项、批次 SDK 6 项、启动契约 8 项通过。测试覆盖 64 位指标、空环境与缺省值、零值、二进制输入输出、未知版本、错误响应、超时预算与不自动重试。Linux 目标全部源码、`sandbox` 与 `sandbox-launcher` 交叉编译链接通过；工具链使用 Zig 和本机为验证编译的 Protobuf 3.21.12，不能代表 Ubuntu 原生构建或运行通过。

Windows 部署模拟的 18 项在受限环境曾有 6 项目录重命名被拒绝；沙箱外两次重试分别有 2 项和 1 项相同的 WinError 5，未能确认整个部署测试套件通过。**本改造尚未进行真实 Linux CTest、socket 集成、批次、隔离、已安装 SDK 和性能测试。**

复制源码到 Ubuntu 后运行：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake python3 libseccomp-dev protobuf-compiler libprotobuf-dev python3-protobuf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

关闭旧会话后按下文的安装升级和 `sandbox-run --test-runtime`、`installed_sdk.py` 流程进行实际隔离验收。性能实验另行启动空闲会话，并传入旧版 CLI：

```bash
python3 tools/session_protocol_benchmark.py --binary ./build/sandbox \
  --legacy-binary /path/to/old/sandbox --state-root /path/to/state-root \
  --run-id run-... --iterations 500
```

记录机器、内核、负载、样本数及输出；未运行前不报告延迟或吞吐结论。对比 A/B/C 时不要把删除 CLI 子进程的收益全部归因于编码格式。

## 0.5 本机任务执行引擎（2026-10-30）

本轮本地验证：7 组可移植 C++ 测试（原 5 组 + task_model、writer）；40 个 Python 用例（session_sdk 9、launch_contract 8、deployment 18、batch_sdk 5）；全部通过。
Linux 运行期源码、sandbox/sandbox-launcher、fixture，以及 session_runtime/session_store/mount_source/output_runtime 测试程序交叉编译链接通过。
**Linux 目标程序没有在本机执行；新增 batch_integration、输出线程和已安装批次路径仍需以下实际验收。**
运行过受控慢写入基准，原始 Windows 数据在 benchmarks/writer-windows-20261030.json；它不是生产吞吐或真实 Linux 取消延迟证据。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/writer_benchmark
./build/sandbox batch --config profiles/batch-process-demo.json --allow-unsafe
```

CTest 现在包含 writer、task_model、output_runtime、batch_sdk、batch_integration，合计 20 项；默认 isolation_integration 可能跳过。
诊断 batch 明确没有隔离，不替代正式安装测试。确认旧会话/批次停止后：

```bash
sudo /usr/bin/python3 tools/deploy.py upgrade --build-dir build --apply
test_root=$(mktemp -d /tmp/sbx-engine.XXXXXX)
python3 /opt/linux-sandbox/share/linux-sandbox/tools/make_runtime.py \
  --fixture "$PWD/build/sandbox-fixture" --output "$test_root/runtime"
/opt/linux-sandbox/bin/sandbox-run --test-runtime "$test_root/runtime"
python3 /opt/linux-sandbox/share/linux-sandbox/tests/acceptance/installed_sdk.py \
  /opt/linux-sandbox/bin/sandbox "$test_root/runtime"
```

除原有三条隔离/session passed 外，委派测试应输出 batch scheduling...passed 与 isolated batch smoke passed。
最后的 SDK 测试额外输出 installed batch SDK automatic delegation passed。
不改变原 AppArmor 授权和全局 sysctl。部署入口保持 LF 首行，部署测试会拒绝 CRLF。

## 0.4 用户提供的 Linux 验收结果（2026-10-29）

常规 CTest：14 项通过，默认隔离项跳过。SDK 自动委派测试通过。
修复 sandbox-run CRLF 后，已安装环境输出 isolated integration passed、process sessions and artifact regression passed、isolated session smoke passed，服务 exit status=0。
这是用户在 Ubuntu 24.04.3 / kernel 7.0.0-38-generic 的真实结果，不是本机代跑。
以下 0.4 “待验收”文字保留历史过程；上述具体成功结果更新相应状态，不能自动覆盖 0.5。


## 0.4 核心结构整理（2026-10-29）

本轮本地已完成：

- 原生 Windows 可移植 C++ 五组：model、metrics、session_model、session_safety、session_contract，通过。
- Python：session_sdk 9 项、launch_contract 6 项、deployment 16 项，合计 31 项通过；部署为临时目录模拟，未改系统策略。
- 全部 C++ 运行期源码及两个主程序通过 Linux x86_64 目标交叉编译和链接；fixture 构建成功。
- session_runtime、session_store、mount_source 三个 Linux 测试程序交叉编译链接成功，**未执行**。
- 新增集成场景：丢弃 execute 首次响应、按相同 ID 重试、不同内容冲突、关闭后查询；以及独立的 installed_sdk.py 自动委派验收入口。**这些新增真实 Linux 场景尚未运行。**

下节 0.3 的真实 Linux 通过记录继续保留，不作为本轮重构已验收的证据。

在 Ubuntu 源码目录运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

应包含 session_contract、session_store、launch_contract，全部常规测试通过。
默认 isolation_integration 可能因没有委派环境而 Skipped；不能把跳过当隔离通过。
确认没有活动已安装会话后升级（使用新版源码里的工具）：

```bash
sudo /usr/bin/python3 tools/deploy.py upgrade --build-dir build --apply
test_root=$(mktemp -d /tmp/sbx-core.XXXXXX)
python3 /opt/linux-sandbox/share/linux-sandbox/tools/make_runtime.py \
  --fixture "$PWD/build/sandbox-fixture" --output "$test_root/runtime"
/opt/linux-sandbox/bin/sandbox-run --test-runtime "$test_root/runtime"
python3 /opt/linux-sandbox/share/linux-sandbox/tests/acceptance/installed_sdk.py \
  /opt/linux-sandbox/bin/sandbox "$test_root/runtime"
```

最后两步用普通用户执行。既有隔离集成应输出三条 passed，新增独立 SDK 测试应输出：
`installed SDK automatic delegation and persistent readiness passed`。
不需要关闭全局 AppArmor 限制，也不需要更换原应用专属策略。


## 最新真实 Linux 隔离验收通过（用户提供输出，2026-10-23）

环境：Ubuntu 24.04.3 虚拟机，内核 7.0.0-38-generic，受管理安装路径 /opt/linux-sandbox。
此前用户 doctor 确认普通 UID 1000、应用专属策略生效、全局 AppArmor userns 限制仍为 1。
本次采用 payload-only upgrade，输出 policy_changed=false、policy_reloaded=false、user_data_removed=false。

用户在普通用户终端运行已安装 sandbox-run --test-runtime，完整输出：

```text
isolated integration passed
process sessions and artifact regression passed
isolated session smoke passed
Finished with result: success
Main processes terminated with: code=exited/status=0
```

该结果确认现有脚本覆盖的隔离单次执行、proc PID 视图、文件边界、网络与 seccomp 基线检查、后代回收、超时、OOM 和指标查询，以及 process 会话回归和 isolated 会话冒烟路径通过。
隔离会话冒烟包含连续命令、工作文件保留、工作区程序执行、目录上传和导出。
测试内容以 tests/acceptance/isolation.py 与 tests/integration/sessions.py 为准；不能据此宣称任意恶意代码安全或完整故障覆盖。

仍待验证：真实卸载、崩溃/掉电恢复、磁盘故障、长期泄漏、并发竞争以及更广泛环境兼容性。
下文“未执行/待验收”描述为此前历史记录，以本节对这些具体测试的最新结论为准。


## procfs 初始化顺序修复

用户已在真实 Linux 确认：mount_source 和部署测试通过，保留原策略升级成功；runtime bind 的 EINVAL 不再出现。
隔离流程继续推进到 procfs，原实现先 pivot_root 并卸载旧根，再 mount proc，返回 EPERM。
这一顺序会失去已有 procfs 的可见性，与内核 mount_too_revealing 约束冲突。

修复为由 P2 在旧挂载树仍可见时，将新 PID namespace 的 procfs 挂到新根/proc，之后 pivot_root 并卸载旧根。
不绑定宿主 proc，不增加权限，不修改原 AppArmor 策略。
fixture 新增 proc-view：核对 /proc/self 与当前 namespace PID 一致、PID 1 可见且旧根不可访问；单次与连续会话均增加相应断言。
这次完整隔离测试仍需用户 Linux 重跑；测试代码存在不代表已经通过。

## 已安装环境的 runtime bind 修复

用户真实反馈：10 项常规 Linux 测试通过；安装和完整性检查成功，普通 UID 1000 的命名空间探测成功，全局 AppArmor 限制仍为 1。
完整隔离测试在 runtime 的第一次 bind mount 返回 EINVAL，尚未通过。

源码中发现源目录 FD 在父 mount namespace 打开，跨 namespace 后直接通过 procfd 绑定；这与内核对源挂载所属命名空间的检查冲突。
修复为在子 namespace 用 openat2 重新打开源目录，拒绝符号链接，核对原 FD 的设备号和 inode，不接受已替换目录。
新增 mount_source Linux 单元测试；此测试及完整隔离路径仍需在用户 Linux 执行。
新增 payload-only upgrade，只在策略逐字一致时更新代码，不重载或扩大策略，拒绝活动任务和被篡改的安装。
16 项部署/升级模拟测试已通过，包括升级不调用策略解析器及拒绝策略变更；mount_source 测试在 Linux 的实际执行仍待确认。

## 固定路径部署与实际启动方式一致的诊断

- 用户已确认：开发目录启动器创建 user namespace 后进入 `unprivileged_userns (enforce)`，探测写 setgroups 被拒绝。
- 新增固定 `/opt/linux-sandbox` 的普通用户部署工具、安装计划、文件清单与 SHA-256、安装/卸载、专属 AppArmor 兼容授权和用户 systemd 入口。
- 不自动修改当前虚拟机或 Windows 系统权限。安装仅在管理员显式执行 `install --apply` 时发生。
- 14 项部署测试在临时目录执行通过，包含载荷完整性、权限规则、路径逃逸拒绝、普通用户入口和模拟策略加载失败回滚；未加载真实 AppArmor 策略。
- 探测改为 clone3 创建命名空间，由父进程写映射，与实际任务保持相同方式；不再使用 unshare 自映射。
- 实际 Ubuntu 策略语法加载、非 root doctor、委派控制器、隔离测试和卸载仍待 Linux 验收。不能将模拟测试当成部署成功。

## 用户真实 Linux 反馈与环境诊断改进

用户在 Ubuntu 24.04.3（内核 7.0.0-38-generic）运行 9 项 CTest：8 项通过，isolation_integration 跳过。
随后显式隔离测试在首次私有挂载操作失败（EACCES），未执行目标；资源组与后代清理成功。
AppArmor 限制开关已开启，但缺少拒绝日志，不能把具体策略来源写成已确认。

本次新增真实启动器的只读环境诊断、委派脚本前置检查和 DEPLOYMENT.md，不更改系统权限。
新增 environment_diagnostics Linux 测试，用于检查诊断响应及调用方命名空间和全局开关不变。
当前 Windows 环境只做构建和语法检查；新探测与隔离路径仍需用户在 Linux 验证。

## SDK 关闭状态竞态修复

用户在真实 Linux 上运行 CTest：model、metrics、session_model、session_safety、session_runtime 和 process_integration 通过；sessions 在 with 退出关闭阶段的状态 RPC 失败；isolation_integration 未配置环境而跳过。

修复：close 接收请求后轮询原子落盘状态，不再请求即将退出的控制端；status 通信失败后只在确认已落盘 closed 时返回终态，否则保留错误。with 清理异常不再覆盖原始执行异常。

新增 session_sdk 确定性回归测试，9 项已在 Windows 执行通过，包括关闭竞态、幂等关闭、真实通信错误保留和原始异常保护。真实 Linux sessions 需同步修复后重新运行，当前不记录为通过。

## 连续会话可靠性修复

- STDIN/STDOUT/STDERR 传 FD 消息显式使用对象 payload；发送端提前拒绝非对象。
- CommandExecution 在临时对象中准备资源，准备失败不会向会话发布半初始化捕获器。
- 已落盘的匹配命令终态优先于旧会话快照；恢复和异常收尾均不覆盖已有终态。
- 指标读取检查对象/数值类型，缺失、null、非法类型和回退计数返回 null；采样异常记录不可用，不关闭会话。
- 新增 session_safety 可移植回归测试，以及 session_runtime Linux 协议/FD 失败回滚测试。
- session_safety 已在 Windows 编译并执行通过：覆盖 null/缺失/类型错误/计数回退、保存终态优先及重复恢复。
- 修复版产品源码和新增 session_runtime 测试已完成 Linux 交叉编译与链接；session_runtime 未执行。
- 准备失败不发布会话资源；已创建的失败命令目录可保留用于诊断，不视为活动命令。
- Linux 运行测试仍须在真实 Linux 执行，交叉编译不视为运行通过。

## 0.3 连续会话与文件交付（2026-10-23）

当前环境仍为 Windows，没有可用 WSL 发行版。

- 原配置/报告模型测试：本次重新编译并执行，通过。
- 原指标解析与采样测试：本次重新编译并执行，通过。
- 新会话模型测试：通过配置往返、非法期限/类型、SHA-256 空串、分块 abc 与百万 a 标准向量。
- 产品源码 Linux x86_64 交叉编译与 sandbox / sandbox-launcher 链接检查：执行，结果随交付记录核验。
- 新增 tests/integration/sessions.py：覆盖连续执行、二进制输入输出、启动失败后复用、取消、超时、输出截断、重连、目录复制与链接拒绝。
- 委派 wrapper 增加 isolated 会话冒烟路径，沿用 SANDBOX_TEST_RUNTIME / SANDBOX_TEST_CGROUP。
- 上述 Linux 集成测试已编写，**未在当前环境执行**。不能把构建通过或测试代码存在当成行为通过。
- OOM、会话到期、SIGKILL 恢复、磁盘故障和竞争条件仍需真实 Linux 验收；暂无完整故障注入覆盖。

完整功能和边界见 PROJECT_GUIDE.md，调用契约见 SESSION_API.md。

## 性能监控借鉴优化（2026-10-17）

- 配置/状态/报告模型测试：通过，新增采样配置范围与往返验证。
- 新增指标解析与采样测试：通过。使用伪 cgroup 文件，覆盖首次、零间隔、回退、大计数精度、多设备、缺失/恢复、有界历史、实例隔离和汇总。
- Python 测试及委派脚本语法、示例 JSON：通过。
- 全部产品源码 Linux x86_64 交叉编译：通过；sandbox 与 sandbox-launcher 链接：通过。
- 真实 Linux 的周期采样、实时 inspect、OOM 与隔离行为仍未运行；已增加相应测试断言，不把测试代码存在当成测试通过。

以下为原始首版验证记录。

日期：2026-10-17。环境：Windows，MinGW g++ 15.1.0；无可用 WSL Linux 发行版。

## 实际通过

1. 本机编译、运行 `tests/unit/model_tests.cpp`：通过。
   覆盖字段拒绝、数值类型、边界、危险模式显式授权、配置往返、状态机非法转换、报告原因优先级与 JSON/UTF-8。
2. 使用 Zig 0.13.0 的 Linux x86_64 / glibc 2.38 目标，编译全部 16 个产品 C++ 翻译单元：通过。
3. `sandbox` 和 `sandbox-launcher` Linux ELF 可执行文件链接：通过。
   使用 Ubuntu 官方 libseccomp-dev 2.5.5 开发包中的静态库做链接检查；该库不进入源码交付包。
4. 静态 `sandbox-fixture` 编译：通过（验证使用 x86_64-linux-musl）。
5. 项目自有 C++ 源码 clang-format 19.1.7 格式检查：通过。
6. 5 个 Python 文件语法检查和 2 个示例 JSON 解析：通过。

## 尚未完成，不能据此宣称通过

- Linux 原生 CMake/CTest 执行。
- namespace、pivot_root、最小设备挂载、无特权 UID/GID 映射的真实行为。
- cgroup 委派、OOM、CPU/pids 限制及整组终止。
- 真实进程竞争、取消时序、强杀恢复和资源泄漏。
- SDK 与运行器端到端执行。
- GitHub Actions。本次只编写工作流，没有上传仓库或运行远端任务。

交叉编译证明源码与目标头文件/库能够编译链接，不证明隔离正确或足以运行恶意程序。

## Linux 接续验收

```bash
sudo apt-get install -y build-essential cmake python3 libseccomp-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

无显式环境参数时隔离测试应 skipped；必须再按 README 准备受控运行时和委派服务，运行隔离测试。隔离设置失败是测试失败，不能改用 process 模式代替隔离验收。

## 交付范围

源码包不含 `work/`、编译缓存或本次下载的测试工具。保留源码、锁定的 JSON/spdlog 依赖、测试、脚本、示例、SDK 和文档。
