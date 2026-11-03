# 测试导航

测试按用途分目录；unit / integration / acceptance / experimental / fixtures，性能工具位于项目 benchmarks/。
分档不是重要性排名：unit、integration、acceptance 都承担回归职责。

## 快速上手：先看六个

1. [integration.py](integration/integration.py)：单次执行、退出、输出与超时。
2. [sessions.py](integration/sessions.py)：持续会话、去重和文件操作。
3. [supervisor_tests.cpp](unit/supervisor_tests.cpp)：执行核心的事件与状态规则。
4. [isolation.py](acceptance/isolation.py)：真实 Linux 隔离。
5. [workspace_quota.py](acceptance/workspace_quota.py)：配额、导出和传输失败，由隔离验收工具调用。
6. [publish_tests.cpp](integration/publish_tests.cpp)：复制、发布、恢复副本和目录回滚。

前五项中实际注册在 CTest 的重点测试，以及 publish，使用 onboarding 标签；workspace_quota.py 不会被该标签运行。

## 分档清单

| 档位 / CTest 标签 | 文件 | 用途 |
|---|---|---|
| 纯逻辑回归 / unit | core_tests.cpp、model_tests.cpp、supervisor_tests.cpp、prepare_tests.cpp、control_tests.cpp、fault_tests.cpp、workspace_tests.cpp、ops_json_tests.cpp、metrics_tests.cpp、session_model_tests.cpp、session_contract_tests.cpp、writer_tests.cpp、task_tests.cpp | 模型、状态、预算、队列和故障规则 |
| SDK 回归 / unit | session_sdk_tests.py、batch_sdk_tests.py | 客户端行为；当前 CMake 在 Linux 分支注册 |
| 系统与组件集成 / integration | control_fd_tests.cpp、output_runtime_tests.cpp、session_store_tests.cpp、mount_source_tests.cpp、session_runtime_tests.cpp、publish_tests.cpp | 真实 FD、输出、文件系统和存储行为 |
| 协议集成 / integration | session_wire_tests.cpp、session_wire_tests.py、session_wire_interop_tests.py | C++ / Python 编解码及互通 |
| 端到端与环境 / integration | integration.py、sessions.py、batch_integration.py、session_prepare.py、environment_diagnostics.py | 实际程序链路、准备失败和环境探测 |
| 部署回归 / deployment | deployment_tests.py、launch_contract_tests.py | 安装布局与启动契约，不等于真实安装验收 |
| 隔离验收 / acceptance | isolation.py | CTest 注册为 isolation_integration；条件不足可返回 77 并跳过 |
| 独立验收 / 不在普通 CTest | workspace_quota.py、installed_sdk.py | 由委派/安装验收流程调用，不能因未注册 CTest 删除 |
| 实验协议 / experimental | wire_tests.cpp | core/wire 二进制编解码；随实现一起决定去留，仍参与默认全套测试 |
| 原型遗留 / 不在 CTest | protobuf_draft_tests.py | 早期协议实验；先核对正式协议测试覆盖，再决定是否合并或移除 |
| 性能工具 / 不在 CTest | writer_benchmark.cpp | 输出写入性能实验，可执行目标 writer_benchmark |
| 测试辅助 / 不单独分档 | fixtures/probe.cpp、session_wire_bridge.cpp | 分别生成 sandbox-fixture、session_wire_bridge，被其他测试依赖 |

portable / linux 是当前构建注册范围标签，不代表每个标记 linux 的测试本身都依赖 Linux 系统调用。

## 按组运行

在项目根目录重新配置，让标签进入构建目录：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --print-labels
ctest --test-dir build -N -L onboarding
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L integration --output-on-failure
ctest --test-dir build -L deployment --output-on-failure
ctest --test-dir build -L experimental --output-on-failure
ctest --test-dir build --output-on-failure
```

`-N` 只列清单，不执行；默认全套仍包含 experimental。Windows 只注册可移植部分，以上命令不会让 Linux 运行时在 Windows 上运行。

完整隔离验收见 [ACCEPTANCE.md](../docs/ACCEPTANCE.md)，入口是 `bash tools/isolated-accept.sh`。该脚本需要 sudo，并临时调整、恢复宿主策略。只运行 `ctest -L acceptance` 不等于完整验收。

## 如何读测试结果

- Skip 表示没执行，不等于通过；重点检查 isolation_integration。
- publish_tests 的整体成功不证明 EXDEV 分支已执行，验收记录须保存 `cross-filesystem publish: exercised` 或 `NOT exercised` 那一行。
- 发布持久性测试中的权限模拟不等于实际磁盘 I/O 故障注入。
- benchmark 的性能数字不替代正确性回归。
- 新增测试时选择一个主要档位；跨组件测试不必因为使用了 C++ 就归为 unit。

## 实际目录

- `unit/`：纯逻辑、SDK 和部署规则回归。
- `integration/`：系统接口、协议互通和完整执行链路。
- `acceptance/`：隔离、配额和安装后验收。
- `experimental/`：实验协议及历史原型。
- `fixtures/`：被测目标和互通辅助程序。
- `../benchmarks/`：性能工具。

目录表示代码位置，CTest 标签表示运行分组；部署规则测试虽位于 unit，仍可通过 deployment 标签运行。
