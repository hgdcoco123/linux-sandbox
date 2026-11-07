# 按顺序读哪些文件

配套 [LEARNING_ROUTE.md](LEARNING_ROUTE.md) 使用：那边是阶段与目标，这里是**逐文件顺序**。
行数是当前版本的真实值，用来估计投入。

**阅读原则**（重要）：

1. **先看形状再看出流程**：先读"数据长什么样"（配置、请求、结果、报告），再读"它怎么流动"。
2. **先读接口再读实现**：`include/sandbox/**/*.hpp` 是契约，`.cpp` 是其中一种满足方式。
3. **先正常后异常**：每个子系统先读一遍成功路径，再读它怎么处理失败。
4. **大文件分段读**：超过 400 行的（`init.cpp`、`run_session.cpp`、`interactive_session.cpp`、
   `wire.cpp`）不要一次读完，按函数分组读。
5. **读一段就停**，用 `ctest -R <label>` 或一条手写命令验证你刚读的东西。

---

## 批次 0：建立行为直觉（不读源码，约 1 小时）

| 顺序 | 文件 | 读什么 |
|---|---|---|
| 1 | `README.md` | 项目解决什么问题，"成功 = 退出 0 且产物已交付"这条语义 |
| 2 | `docs/QUICKSTART.md` | 跟随两条路线跑一遍 |
| 3 | `docs/USAGE.md` §1–5 | 四个输入、四种 verdict、运行目录布局 |
| 4 | `docs/CONTRACT.md` | 哪些字段是稳定承诺（后面读代码会看到它们从哪来） |

跑：`./build/sandbox run --process --quiet --summary -- /bin/sh -c 'echo hi; exit 3'`，然后
`./build/sandbox inspect <run_id>`，再 `cat /tmp/sbx-$(id -u)/<run_id>/checksums.json`。

---

## 批次 1：数据形状与判定（约 900 行，1 天）

这一批决定后面所有批次的理解速度：**输入是什么、输出是什么、判定怎么算**。

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/core/ids.hpp` | 58 | 为什么 RunId / SessionId 是不同类型 |
| 2 | `include/sandbox/model/config.hpp` | 64 | 配置字段清单（限制、挂载、workspace、seccomp） |
| 3 | `src/model/config.cpp` | 283 | 解析 + 校验：哪些组合被拒绝，为什么 |
| 4 | `include/sandbox/model/report.hpp` | 61 | 报告形状（`schema_version`、verdict、`workspace.export`、timing） |
| 5 | `src/model/report.cpp` | 117 | 报告怎么从内部结果生成 |
| 6 | `include/sandbox/core/result.hpp` | 94 | **结果三分**：outcome / output / cleanup |
| 7 | `include/sandbox/core/outcome.hpp` | 86 | verdict 的输入是什么 |
| 8 | `src/core/outcome.cpp` | 62 | **verdict 判定表**——整个项目最重要的一小段 |
| 9 | `include/sandbox/core/request.hpp` + `src/core/request.cpp` | 51 + 78 | 请求如何被规范化 |
| 10 | `apps/cli/main.cpp` | 374 | 参数 → 请求 → 报告打印；`print_summary` 的七行从哪来 |

**读完要能画出**：`配置 + 命令 → 请求 → (执行) → outcome + output + cleanup → 报告`。

---

## 批次 2：一次运行的骨架（约 600 行，半天）

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/application/service.hpp` + `src/application/service.cpp` | 18 + 112 | 用例编排：准备 → 执行 → 发布 → 报告 |
| 2 | `include/sandbox/core/supervisor.hpp` + `src/core/supervisor.cpp` | 117 + 268 | 谁负责等待、超时、回收 |
| 3 | `include/sandbox/core/prepare.hpp` + `src/core/prepare.cpp` | 88 + 133 | **正向准备 + 逆序回滚**：第 N 步失败怎么撤 |
| 4 | `include/sandbox/core/workspace.hpp` + `src/core/workspace.cpp` | 67 + 74 | tmpfs 配额（字节 / inode） |

---

## 批次 3：跨进程契约（约 1200 行，1~2 天）

沙箱真正的复杂度在**进程之间**。这一批顺序不能乱：先控制消息，再线协议，再启动器。

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/core/control.hpp` | 172 | 控制消息类型清单（launcher/init 之间的语言） |
| 2 | `src/core/control.cpp` | 371 | 消息怎么编码、怎么处理、错误如何回应 |
| 3 | `include/sandbox/core/wire.hpp` | 31 | 线格式约束（长度、边界） |
| 4 | `src/core/wire.cpp` | 509 | **分段读**：帧解析、短读/短写、EOF、上限 |
| 5 | `include/sandbox/core/ops.hpp` + `src/core/ops.cpp` | 132 + 69 | 动作（ops）的类型 |
| 6 | `src/core/ops_json.cpp` | 282 | JSON ↔ ops（允许出现 JSON 的边界之一） |
| 7 | `include/sandbox/runtime/launcher.hpp` + `src/runtime/launcher_client.cpp` | 31 + 149 | CLI 如何 fork 并交接 fd |
| 8 | `apps/launcher/main.cpp` | 113 | launcher 本体：`unshare`、exec、超时 |
| 9 | `include/sandbox/runtime/protocol.hpp` + `src/runtime/protocol.cpp` | 32 + 83 | 单次运行的控制通道用法 |

**读完要能回答**：一个 fd 从 CLI 传到 target 要经过几次交接、哪些必须关闭。

---

## 批次 4：PID 1 与隔离（约 800 行，1~2 天）

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/runtime/init_common.hpp` + `src/runtime/init_common.cpp` | 39 + 22 | PID 1 的共享骨架 |
| 2 | `src/runtime/init.cpp` | 569 | **分段读**：① seccomp/权限 ② 关 fd ③ exec 目标 ④ 回收子进程 ⑤ 导出前收尾 |
| 3 | `include/sandbox/isolation/isolation.hpp` + `mount_source.hpp` | 22 + 31 | 隔离边界涉及哪些对象 |
| 4 | `src/isolation/rootfs.cpp` | 106 | mount、pivot_root/chroot、只读绑定 |
| 5 | `src/isolation/security.cpp` | 121 | seccomp 基线策略与拒绝名单 |

**练习**：注释掉 `security.cpp` 里一条规则 → 跑 `tests/acceptance/isolation.py` → 看哪条断言变红 → 恢复。

---

## 批次 5：把一条命令的生命周期串起来（约 1500 行，2 天）

最值得反复读的一段：**前面所有批次在这里合流**。

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/runtime/command_execution.hpp` | 49 | 命令执行对象的所有权 |
| 2 | `src/runtime/run_session.cpp` | 898 | **按函数读**：准备 → 启动 → 读输出 → 等结束 → 导出 → 判定 → 清理 |
| 3 | `include/sandbox/runtime/output.hpp` + `src/runtime/output_capture.cpp` | 29 + 123 | 输出如何落地；EOF / 短写 / ENOSPC |
| 4 | `include/sandbox/runtime/bounded_writer.hpp` + `src/runtime/bounded_writer.cpp` | 51 + 105 | 背压：队列满时拒绝而不是阻塞 |
| 5 | `src/platform/linux.cpp` | 250 | 系统调用的薄封装（read/write/wait 语义） |
| 6 | `src/platform/environment.cpp` | 190 | 环境探测（`doctor` 的很多字段来自这里） |
| 7 | `src/platform/event_loop.cpp` | 43 | epoll 等待入口 |

**读法**：第一遍只读 `run_session.cpp` 的函数签名与注释，把主流程图写下来；第二遍才进函数体。

---

## 批次 6：资源、交付与自证（约 1000 行，1~2 天）

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/resources/cgroup.hpp` + `src/resources/cgroup.cpp` | 21 + 108 | 委派检查、限额写入、清理 |
| 2 | `include/sandbox/storage/artifacts.hpp` | 67 | 发布接口（staging / exchange / manifest） |
| 3 | `src/storage/artifacts.cpp` | 458 | **原子发布**：`RENAME_EXCHANGE`、跨文件系统 copy、manifest |
| 4 | `include/sandbox/storage/sha256.hpp` | 89 | 校验和（含 `O_NOFOLLOW`） |
| 5 | `include/sandbox/storage/store.hpp` + `src/storage/run_store.cpp` | 70 + 457 | 状态目录布局、`write_checksums`、`prune`、`cleanup` |
| 6 | `src/resources/metrics.cpp` + `include/sandbox/resources/metrics.hpp` | 263 + 51 | 指标口径（哪些数字值得信） |
| 7 | `src/core/record_json.cpp` | 249 | 持久化记录编码（v3 与兼容） |

---

## 批次 7：会话与批处理（约 2500 行，2~3 天）

量大，但模式与批次 5 相同：只是"一次"变成"多次"，多了状态机与持久化。

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `include/sandbox/runtime/session_state.hpp` | 58 | 状态机：合法迁移与拒绝 |
| 2 | `include/sandbox/runtime/session.hpp` + `session_policy.hpp` | 76 + 37 | 会话对象与策略（期限、并发、配额） |
| 3 | `src/runtime/interactive_session.cpp` | 1121 | **分段读**：按状态机每个状态读一段 |
| 4 | `include/sandbox/runtime/session_protocol.hpp` + `src/runtime/session_protocol.cpp` | 64 + 74 | 会话控制消息 |
| 5 | `include/sandbox/runtime/session_wire.hpp` + `src/runtime/session_wire.cpp` | 40 + 455 | 会话线协议（protobuf 编解码边界） |
| 6 | `include/sandbox/storage/session_store.hpp` + `src/storage/session_store.cpp` | 59 + 322 | 会话持久化、去重、checksums |
| 7 | `include/sandbox/model/task.hpp` + `src/model/task.cpp` | 59 + 144 | 批次任务模型 |
| 8 | `src/runtime/batch.cpp` | 343 | 批次调度与汇总 |
| 9 | `proto/session_v1.proto` | — | 线协议定义（与第 5 项对照读） |
| 10 | `sdk/session_client.py` | — | 客户端角度：调用者看到什么 |

---

## 批次 8：分析层（约 1900 行 Python，2 天）

没有 C++ 的复杂度，但**判定与证据的思想密度最高**。

| 顺序 | 文件 | 行数 | 读什么 |
|---|---|---|---|
| 1 | `sandbox_ai/models.py` | 56 | 数据模型（证据、引用、分析结果） |
| 2 | `sandbox_ai/evidence.py` | 394 | 行对齐分块、行号前缀、`verify_provenance` |
| 3 | `sandbox_ai/service.py` | 498 | 检索（BM25）、引用解析与拒绝、工具循环、`judge()` |
| 4 | `sandbox_ai/store.py` | 365 | SQLite 模式、语料代际缓存、向量 blob、单写者锁 |
| 5 | `sandbox_ai/verification.py` | 172 | 模板 digest 锁定、`${run_dir}`、回执语义 |
| 6 | `sandbox_ai/providers.py` | 216 | 提供方适配、重试与心跳 |
| 7 | `sandbox_ai/cli.py` | 251 | 子命令与配置来源 |
| 8 | `tests/unit/ai/test_links.py` | 243 | **引用契约测试**（与第 3 项对照读） |
| 9 | `tests/unit/ai/test_analysis.py` | 436 | 分析结果结构 |
| 10 | `tests/unit/ai/test_verification.py` | 117 | 受控验证 |
| 11 | `docs/AI_ANALYSIS.md`、`docs/VALUE.md`、`docs/AI_VALIDATION.md` | — | 指标与样本边界 |

---

## 批次 9：测试与交付（1 天）

| 顺序 | 文件 | 读什么 |
|---|---|---|
| 1 | `CMakeLists.txt` | `add_test` 清单与 `LABELS`（portable / experimental）；`SANDBOX_STATIC_DEPS` |
| 2 | `tests/integration/integration.py` | 单次运行的集成测试怎么写 |
| 3 | `tests/acceptance/isolation.py` | 隔离验收：为什么需要 `SANDBOX_TEST_*` |
| 4 | `tests/acceptance/workspace_quota.py` | 配额类断言怎么写 |
| 5 | `tools/delegated_run.py` | 在委派单元里跑验收的驱动 |
| 6 | `tools/cgroup_setup.py` | 无 systemd 时怎么自己造 cgroup 子树 |
| 7 | `tools/ci_run.py` + `action.yml` | CI 入口：路径、环境变量、退出码 |
| 8 | `.github/workflows/linux.yml` / `action-smoke.yml` / `release.yml` | 三道门禁各验证什么 |
| 9 | `tools/pack.sh` + `tools/install.sh` | 发布包如何生成、为什么静态链接 |

---

## 如果只有 3 小时

按这个最小集：

1. `README.md`
2. `docs/USAGE.md` §1–5
3. `src/core/outcome.cpp`（62 行，判定表）
4. `include/sandbox/core/result.hpp`
5. `src/runtime/run_session.cpp` 的**函数签名与注释**（不读函数体）
6. `src/storage/artifacts.cpp` 的发布部分
7. `sandbox_ai/evidence.py` 的 `spans()` + `verify_provenance()`
8. `sandbox_ai/service.py` 的引用校验部分

这八处读完，就知道这个项目在哪些地方比"套个容器"更认真。

## 阅读顺序图

```
批次0 行为直觉
   ↓
批次1 数据形状 + verdict ──────────┐
   ↓                              │
批次2 一次运行的骨架                │
   ↓                              │
批次3 跨进程契约 ─→ 批次4 PID1/隔离 │
   ↓                              │
批次5 生命周期串起来 ←──────────────┘
   ↓
批次6 资源 / 交付 / 自证
   ↓
批次7 会话与批处理（同模式的扩展）
   ↓
批次8 分析层（独立可学）
   ↓
批次9 测试与交付
```
