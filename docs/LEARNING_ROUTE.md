# 学习路线

目标不是"读过所有文件"，而是**能独立改一处、并能证明没改坏**。每一阶段都给：读什么、跑什么、
回答什么问题、做一个什么练习。做完一个阶段再进下一阶段，别跳。

前置：C++20 基础（RAII、`std::filesystem`、`std::optional`、`std::chrono`、多线程基本概念）、
Linux 基础（进程、fd、namespace、cgroup 概念）、会读 `cmake`。

---

## 阶段 0：把它跑起来，看懂输出（半天）

**读**：`README.md` → `docs/QUICKSTART.md` → `docs/USAGE.md` 第 1~4 节。

**跑**：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel
./build/sandbox doctor
./build/sandbox run --process --quiet --summary -- /bin/sh -c 'echo hi; exit 3'
./build/sandbox inspect <run_id>
ls /tmp/sbx-$(id -u)/<run_id>
cat /tmp/sbx-$(id -u)/<run_id>/checksums.json
```

**要能回答**：

1. `verdict` 与 `exit:`（沙箱进程退出码）为什么必须分开？
2. 一次成功运行会在运行目录里留下哪几个文件，各自的用途？
3. `--process` 与隔离模式在报告里差哪个字段？

**练习**：用 `--process` 跑一个想把文件写到 `/output` 的命令，观察 `artifacts` 那一行为什么不出现。

---

## 阶段 1：判定语义——这个项目最核心的设计（1 天）

**读**：`include/sandbox/model/*.hpp` → `src/model/config.cpp` → `src/core/outcome.cpp` →
`src/application/service.cpp`，重点找 `verdict` 是怎么算出来的。

**跑**：

```bash
ctest --test-dir build -R "core|fault" --output-on-failure
```

**要能回答**：

1. `ok` / `target_failed` / `harness_error` / `cleanup_incomplete` 的边界分别由谁决定？
2. "产物没交付"为什么不是 `ok`？这个判断写在哪一行？
3. 清理失败为什么要把 verdict 降级，而不是只写日志？

**练习**：给 `tests/unit/fault_tests.cpp` 加一个用例，构造"目标正常退出但导出失败"，断言 verdict
不是 `ok`。跑通、提交、再删掉——目的是熟悉测试怎么加。

---

## 阶段 2：进程架构与隔离（2~3 天，最难）

**读**（按顺序）：

1. `apps/cli/main.cpp`——参数怎么变成请求；
2. `src/runtime/run_session.cpp`——一次运行的生命周期；
3. `src/runtime/launcher_client.cpp` + `apps/launcher/main.cpp`——fork/exec、fd 传递；
4. `src/runtime/init.cpp` + `src/runtime/init_common.cpp`——PID 1 做什么；
5. `src/isolation/rootfs.cpp`、`src/isolation/security.cpp`——namespace、mount、seccomp；
6. `src/core/control.cpp`——控制消息契约。

**跑**：

```bash
./build/sandbox doctor                 # 隔离可用性；不可用时它会说卡在哪个 stage
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output /tmp/rt
```

**要能回答**：

1. 为什么需要独立的 launcher 进程，而不是 CLI 自己 `unshare`？
2. fd 在 CLI → launcher → init → target 之间怎么交接，哪些必须关？
3. 目标进程为什么是 PID 2？PID 1 负责什么？
4. 隔离失败时，为什么报告能区分"目标启动失败"和"沙箱准备失败"？

**练习**：注释掉 `src/isolation/security.cpp` 里的一条 seccomp 规则，跑验收测试看哪条断言变红，
再恢复。这一步能让你明白"策略"和"测试"是怎么对应的。

---

## 阶段 3：资源与准备/回滚（1~2 天）

**读**：`src/core/prepare.cpp`（正向准备 + 逆序回滚）、`src/resources/cgroup.cpp`（cgroup v2 写入、
委派要求）、`src/core/workspace.cpp`（tmpfs 配额）、`src/metrics/*`。

**要能回答**：

1. 准备步骤为什么必须可逆？第 3 步失败时前 2 步怎么撤？
2. 为什么"父 cgroup 里还有进程"会让写 `cgroup.subtree_control` 失败？
3. tmpfs 的字节配额与 inode 配额分别防什么？

**练习**：在 `prepare.cpp` 中间插一个必然失败的步骤，跑一个用例，确认回滚顺序正确。

---

## 阶段 4：产物交付（1 天，值得细看）

**读**：`src/storage/artifacts.cpp`——staging、`renameat2(RENAME_EXCHANGE)`、跨文件系统
copy-then-exchange、`artifact-manifest.json`；再看 `src/runtime/run_session.cpp` 的 `publish_workspace()`。

**要能回答**：

1. 为什么不能直接 `rename` 覆盖？`RENAME_EXCHANGE` 多解决了什么？
2. `durable` 与 `retained_previous_path` 分别回答什么问题？
3. 发布失败时宿主目录处于什么状态？为什么这样是安全的？

**练习**：读 `tests/integration/` 里发布相关用例，画出"成功 / 目标失败 / 发布失败"三条路径的目录终态。

---

## 阶段 5：存储、自证与运维（1 天）

**读**：`src/storage/run_store.cpp`（状态目录、`write_checksums`、`prune`）、`src/storage/session_store.cpp`、
`src/core/record_json.cpp`，以及 `docs/CONTRACT.md`。

**要能回答**：

1. `checksums.json` 覆盖哪些文件？为什么它防误改但不防伪造？
2. `prune` 为什么只删已结束的运行？`cleanup` 为什么只回收资源不删文件？
3. 报告字段分级（稳定 / 实验 / 诊断）怎么落到代码里？

**练习**：改运行目录里 `stdout.bin` 的一个字节，再用分析层 `import` 该运行，观察它拒绝并说明原因。

---

## 阶段 6：会话与批处理（1~2 天）

**读**：`src/runtime/interactive_session.cpp`、`src/core/supervisor.cpp`、`proto/session_v1.proto`、
`sdk/session_client.py`、`tests/integration/sessions.py`。

**要能回答**：

1. 会话里每条命令为什么也要有自己的 `checksums.json`？
2. 命令的"结束"为什么要分成执行结束、写入完成、EOF 三个事实？
3. protobuf 消息里哪些字段是幂等键，为什么需要？

**练习**：用 `SessionClient` 写三步脚本（写文件 → 读文件 → 故意失败），把三步报告都取出来比对。

---

## 阶段 7：分析层（2 天，纯 Python，可单独学）

**读**（按顺序）：

1. `sandbox_ai/evidence.py`——证据分块（行对齐、不重叠）、行号前缀、`verify_provenance`；
2. `sandbox_ai/service.py`——BM25 检索、引用解析、工具循环、`--self-check`、`judge()`；
3. `sandbox_ai/store.py`——SQLite 模式、语料代际缓存、向量 blob、单写者锁；
4. `sandbox_ai/verification.py`——模板 digest 锁定、`${run_dir}` 替换、回执语义；
5. `docs/AI_ANALYSIS.md`、`docs/AI_VALIDATION.md`（哪些结论有测量支撑）。

**跑**：

```bash
python3 -m unittest discover -s tests/unit/ai       # 108 项
bash tools/ai-check.sh                              # 契约 + demo + 指标 + 打包元数据
PYTHONPATH=. python3 -m sandbox_ai --provider demo --db /tmp/a.db --scope demo demo --output /tmp/o
```

**要能回答**：

1. 模型为什么"无法伪造引用文本"？这个保证由哪几行代码实现？
2. 行边界对齐分块、不重叠，各自解决什么问题？
3. 为什么 `hypothesis_updates` 的"支持/反驳"必须引用验证运行的证据？
4. 语义充分性的指标现在有多可信？（看 `docs/VALUE.md`、`AI_VALIDATION.md`，注意样本量）

**练习**：把 `evidence.spans()` 的目标块大小从 1600 改到 300，跑 108 项测试，看哪些用例变红——
它们就是保护分块语义的地方。

---

## 阶段 8：测试体系与 CI（1 天）

**读**：`CMakeLists.txt` 里 `add_test` 的注册与 `LABELS`（`portable` / `experimental`）、
`tests/integration/integration.py`、`tests/acceptance/isolation.py`、`tools/delegated_run.py`、
`.github/workflows/linux.yml`、`.github/workflows/release.yml`、`action.yml`。

**要能回答**：

1. 为什么 `isolation_integration` 在缺 `SANDBOX_TEST_RUNTIME`/`SANDBOX_TEST_CGROUP` 时是 skip？
   34/34 全绿能不能说明隔离被验证过？
2. 托管 runner 上为什么 `sudo systemd-run --system` 能隔离，而 `--user` 不行？
3. `action.yml` 为什么必须给 unit 传绝对路径？（提示：`systemd-run` 不继承调用者环境）

**练习**：把某个断言改松，跑 `bash verify.sh` 看它是否仍然全绿——如果仍然绿，说明那个位置本来就没有
测试保护。找到真正会变红的地方。

---

## 阶段 9：发布与交付（半天）

**读**：`tools/pack.sh`、`tools/install.sh`、`docs/RELEASING.md`、`docs/PUBLISH.md`、
`.github/workflows/release.yml`、`CMakeLists.txt` 里的 `SANDBOX_STATIC_DEPS`。

**要能回答**：

1. 为什么发布包要静态链接 libseccomp/protobuf？glibc 依赖为什么解决不了？
2. `sandbox-ai` 的包装脚本为什么按自身位置解析 `PYTHONPATH`？
3. tag push 之后 release workflow 做了什么？哪一步可能"发出去一个没测过的包"？

**练习**：`SBX_BUILD_DIR=build-static bash tools/pack.sh`，解开包，`ldd` 看依赖，再用 `objdump -T`
查它要求的最高 glibc 符号版本。

---

## 建议节奏

| 周 | 内容 | 产出 |
|---|---|---|
| 第 1 周 | 阶段 0–2 | 能画出"一次运行"的进程与 fd 时序图 |
| 第 2 周 | 阶段 3–5 | 能解释三条失败路径的目录终态 |
| 第 3 周 | 阶段 6–7 | 能跑通会话 + 分析层，并说清引用契约 |
| 第 4 周 | 阶段 8–9 | 能加一个测试、能发一个版本 |

## 三个容易踩的坑

1. **不要从 `src/` 全量读起**。按阶段走，每次只碰一条链路；这个仓库有价值的不是代码量，是判断依据。
2. **不要把"测试绿了"当成"理解了"**。每个阶段末尾的"要能回答"才是验收标准。
3. **不要忽略文档里的边界**。`THREAT_MODEL.md`、`CONTRACT.md`、`AI_VALIDATION.md` 里的"不防什么 / 未验证"
   和功能一样是设计的一部分。

## 配套文档

- 状态与交接：`docs/HANDOFF.md`
- 架构：`docs/ARCHITECTURE.md`、`docs/CORE_STRUCTURE.md`
- 报告字段：`docs/CONTRACT.md`
- 安全边界：`docs/THREAT_MODEL.md`
- 分析层：`docs/AI_ANALYSIS.md`、`docs/AI_VALIDATION.md`、`docs/VALUE.md`
- 发布：`docs/RELEASING.md`、`docs/PUBLISH.md`
- 历史设计笔记：`docs/REDESIGN.md`、`docs/REMAINING.md`、`docs/TASK_ENGINE.md`、`docs/RETROSPECTIVE.md`
