# 交接文档

写给下一个接手的会话（或未来的自己）。读完这份应该知道：**现在是什么状态、怎么验证、改哪里、
哪些坑已经踩过、还剩什么**。

- 想从零学这个项目 → 先看 [LEARNING_ROUTE.md](LEARNING_ROUTE.md)。
- 想看某个设计为什么这么定 → 看 [ARCHITECTURE.md](ARCHITECTURE.md)、[REDESIGN.md](REDESIGN.md)、
  [CONTRACT.md](CONTRACT.md)、[THREAT_MODEL.md](THREAT_MODEL.md)。
- 历史交接（0.4/0.5 轮的内部重构、配额决策）见 `git log` 与 `docs/HANDOFF_QUOTA.md`、`docs/REMAINING.md`。

## 1. 当前状态

| 项 | 值 |
|---|---|
| 公开仓库 | `https://github.com/hgdcoco123/linux-sandbox`（**权威**） |
| 分支 | `master` |
| 最新 tag / Release | `v0.5.0`（首个）、`v0.5.1`（可移植预编译包） |
| 版本号 | `CMakeLists.txt` 的 `project(... VERSION 0.5.1)` 与 `pyproject.toml` 一致 |
| C++ 测试 | `ctest --test-dir build` → **34/34**（`isolation_integration` 缺 `SANDBOX_TEST_*` 时 skip，**skip 不算验证**） |
| 分析层测试 | `python3 -m unittest discover -s tests/unit/ai` → **108 项** |
| 一键自检 | `bash verify.sh`（CTest + 契约 + demo + 打包元数据） |
| 隔离演练 | `bash tools/ai-verify-e2e.sh`（需要 sudo 放宽 `apparmor_restrict_unprivileged_userns`，脚本自动恢复） |
| GitHub CI | `linux.yml` 绿；`action-smoke.yml` 绿；`release.yml` 由 tag 触发并发布 Release |

### 1.1 本地克隆与远端的关系（容易踩）

开发机（Windows）无法稳定访问 `github.com`（HTTPS 常被 reset），因此**本地克隆可能落后于
`origin/master`**，且本地存在内容相同的镜像提交。判断"真实状态"一律以 GitHub 为准：

```bash
git fetch origin && git reset --hard origin/master
```

## 2. 这个项目是什么（一句话版本）

Linux 上跑不可信程序的执行底座：返回**判定**（这次算不算成功）与**产物清单**（交付了什么、能不能核验），
其上有一个分析层把失败翻译成"发生了什么 / 缺什么 / 下一步"，每条结论都能指回某一行证据。

## 3. 代码地图

```
apps/cli/main.cpp              CLI：参数 → 请求；报告打印；doctor/inspect/prune/cleanup
apps/launcher/main.cpp         独立 launcher：fork init、fd 传递、超时与回收
include/sandbox/core/          不依赖 JSON 的业务内核（类型化 ID、结果三分、执行核心、准备/回滚）
include/sandbox/model/         配置与报告的模型层（唯一允许出现 JSON 的地方之一）
src/core/                      内核实现（outcome/result/request/ops/wire/supervisor/prepare/control/workspace）
src/runtime/                   执行引擎：run_session（单次）、interactive_session（会话）、init（PID 1）、
                               protocol（控制通道）、output_capture、bounded_writer
src/isolation/                 rootfs（mount/namespace）、security（seccomp）
src/resources/cgroup.cpp       cgroup v2：委派、限额、清理
src/storage/                   run_store（状态目录、checksums、prune）、session_store、artifacts（原子发布）
src/application/service.cpp    用例编排层（一次运行的完整流程）
sandbox_ai/                    分析层：evidence（分块/引用）、service（检索/工具循环/judge）、store（SQLite）、
                               verification（受控验证）、models、providers、cli
sdk/                           Python SDK（session_client），proto/session_v1.proto 为其线协议
tests/unit|integration|acceptance|fixtures
tools/                         make_runtime / delegated_run / ci_run / cgroup_setup / pack / install / ai-*
```

## 4. 关键设计决策（改代码前必须知道）

| 决策 | 为什么 | 在哪体现 |
|---|---|---|
| `verdict` 与 `summary` 分离 | "目标失败"和"沙箱故障"必须能区分，否则 CI 会误报 | `src/core/outcome.cpp`、报告 `schema_version: 2` |
| 产物未交付 ≠ 成功 | 退出码 0 但导出失败是最危险的假阳性 | `run_session` 的发布与判定收尾 |
| 产物用 staging + `RENAME_EXCHANGE` | 崩溃时不能出现半新半旧的目录；跨文件系统则 copy-then-exchange | `src/storage/artifacts.cpp` |
| 每次运行/每条会话命令都写 `checksums.json` | 让"证据是否被改动"可判定；分析层据此拒绝导入 | `run_store.cpp` 的 `write_checksums`、`session_store` |
| 分析层只接受 `{id, line}` 引用 | 让模型无法书写引用文本，伪造无从下手 | `sandbox_ai/service.py` 的解析与校验 |
| 证据按行对齐、不重叠分块 | 引用必须能精确指行；重叠会让同一行有两个编号 | `sandbox_ai/evidence.py` 的 `spans()` |
| 受控验证必须用登记模板（digest 锁定） | 让"验证"可复现且不被悄悄改写 | `sandbox_ai/verification.py` |
| `--process` 明确不是安全边界 | 环境不允许隔离时要能跑通链路，但不能假装隔离 | `doctor`、报告 `isolated` 字段 |
| 预编译包静态链接 seccomp/protobuf | 否则只能在"恰好有匹配 soname"的机器上运行 | `CMakeLists.txt` 的 `SANDBOX_STATIC_DEPS` |

## 5. 已踩过的坑（别重复）

| 坑 | 现象 | 结论 |
|---|---|---|
| `systemd-run` 不继承调用者环境 | unit 里 `GITHUB_WORKSPACE` 为空，报告写到 `$HOME` | 传给 unit 的路径**必须绝对** |
| user 级委派拿不到控制器 | `prepare()` 失败：`cgroup.controllers` 缺 cpu/memory/pids | 托管 runner 上用 `sudo systemd-run --system`；本地用 `systemd-run --user -p Delegate=yes` |
| 父 cgroup 里还有进程 | 写 `cgroup.subtree_control` 返回 EBUSY | 先把自身移入子 cgroup 再委派（`tools/delegated_cgroup.py`） |
| 34/34 不代表隔离被验证 | `isolation_integration` 缺环境变量时 skip | 要真验证必须跑 `tools/ai-verify-e2e.sh` |
| `result.json` 与 `checksums.json` 非原子 | 读得太快会看不到 checksums | 读取方应短暂重试（测试里已如此），`verify_provenance` 缺失记为 `unverified` |
| Ubuntu 24.04 的 PEP 668 | `pip install .` 报 externally-managed-environment | 用 venv，或 `--break-system-packages`（只装用户目录时） |
| 预编译包只有 glibc 依赖 | 静态链接后仍需 `GLIBC_2.38`/`GLIBCXX_3.4.32` | 老系统（22.04）需从源码构建；要兼容得在旧容器里构建 |
| macOS/Windows | 只建 model 测试，运行时不可用 | 平台边界写在 README |

## 6. 怎么验证（按成本从低到高）

```bash
# 1) 快速：C++ 单元/集成（不含隔离）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 2) 分析层
python3 -m unittest discover -s tests/unit/ai
bash tools/ai-check.sh

# 3) 一键（含上面两项 + demo + 打包元数据）
bash verify.sh

# 4) 真隔离（需要放宽 userns，脚本自动恢复）
bash tools/ai-verify-e2e.sh

# 5) 单独跑隔离验收（已有一个可委派的 cgroup 时）
python3 tests/acceptance/isolation.py build/sandbox     # 需 SANDBOX_TEST_RUNTIME / SANDBOX_TEST_CGROUP
```

**判定标准**：`verify.sh` 绿只说明"可移植部分没坏"；隔离结论只能来自第 4 或第 5 项。

## 7. 还剩什么（都不是 bug，是需要环境或超出范围）

**需要别的环境才能做**

- 在 **Ubuntu 22.04 容器**里构建发布包，把兼容面从 24.04+ 扩到 22.04+（改 `release.yml` 加一个 job）。
- 在**真实 GitHub 仓库**里验证 `uses:` 的完整外壳（当前机制已验证，外壳未验证）。

**明确不做（写进文档的边界）**

- 多租户与鉴权、seccomp 策略 DSL、常驻守护进程、分布式调度、ANN 索引。
- 报告 JSON Schema 与 golden 快照、两套生命周期状态机的合并。

**已知的诚实短板**

- 分析层语义充分性只有自评指标（`docs/VALUE.md`，样本 n=3、单模型）。
- 引用通过率是样本内结果（`docs/AI_VALIDATION.md`）。
- `checksums.json` 自证：防误改、不防伪造（无签名）。

## 8. 开发环境（这次会话用的那套）

| 项 | 值 |
|---|---|
| 编辑 | Windows，`sbx/`（工作副本）；另有 `D:\cpp_project\linux-sandbox` 作为交付副本 |
| 构建/测试 | Ubuntu 24.04 虚拟机（x86_64），`~/sbx3`（解包工作树）与 `~/linux-sandbox`（git 克隆） |
| 传输 | 自定义 HTTP 桥（`POST /put/f/<path>` 上传、`POST /exec` 执行），无共享目录 |
| 凭据 | `~/.sandbox-ai.env`（不入库；用完 `shred -u`） |
| 已知限制 | 开发机到 github.com 的 HTTPS 不稳定；推/拉由 VM 完成 |

在**别的环境**接手时可以忽略这一节，只要 `bash verify.sh` 绿即可。

## 9. 新会话怎么开始（把这段直接粘贴过去）

> 这是一个 Linux 沙箱 + 分析层的项目，仓库在 `sbx/`（或 `https://github.com/hgdcoco123/linux-sandbox`），
> 当前 v0.5.1，`bash verify.sh` 应全绿（34 项 CTest + 108 项分析层测试），隔离结论以
> `bash tools/ai-verify-e2e.sh` 为准（需一次 sudo 放宽 userns，脚本自动恢复）。
> 请先读 `docs/HANDOFF.md`（状态、决策、坑）和 `docs/CONTRACT.md`（报告字段承诺），
> 再改任何代码；改报告字段前必须看 `CONTRACT.md` 的分级规则。
> 本次要做的第一件事是：<你的具体任务>。

## 10. 一条经验

这个项目里**每个"已完成"都要有对应证据**：一条测试名、一次真机输出、或一个测量数字。
写文档时也一样——"未验证"本身是可交付的信息，比一句乐观的断言更有用。
