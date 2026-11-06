# AI 分析层验证记录

日期：2026-11-06。环境：Linux（Ubuntu 虚拟机），Python 3.12；Windows 上另行跑过同一套测试。

## 已执行

在仓库根目录执行：

```text
python3 -m unittest discover -s tests/unit/ai -v
```
结果：**49 项全部通过**（证据 15、分析 14、检索 5、传输 5、验证 6、新增链路 4，按文件计
`test_analysis.py` 29 / `test_links.py` 9 / `test_transport.py` 5 / `test_verification.py` 6）。
Windows 上符号链接用例会跳过（系统不允许普通用户创建），Linux 上全部执行。

同一套测试已注册进 CTest：

```text
ctest --test-dir build
```

`ai_contracts` 作为第 34 个测试运行并通过（`100% tests passed, 0 tests failed out of 34`；
`isolation_integration` 在无 systemd 委派时跳过）。

`tools/ai-check.sh` 在任意 Python 3 主机上串起三件事：跑完 49 项契约测试、跑离线 demo 并断言
三次分析均为 `completed`、再用 `stats` 断言 `structured_output_rate == 1.0` 与 `tool_denied == 0`。
输出末行为 `AI CHECK PASSED: contracts, demo and aggregate metrics`。

`bash tools/ai-verify-e2e.sh --dry-run` 已执行：配置生成、模板登记、摘要锁定这三步通过，
输出 `AI VERIFY E2E DRY RUN OK`。**完整链路（真实隔离执行）尚未执行**——它需要操作者本人运行
`bash tools/ai-verify-e2e.sh`（脚本内部调 sudo 放行/恢复 sysctl），见下方"未验证"。

离线 demo 仍生成编译失败、CSV 结构问题、超时三类**合成**证据；`DemoProvider` 是确定性测试替身，
不代表 AI 诊断效果。

## 覆盖范围

- 原始执行结果不被模型更改；模型故障、预算耗尽仍保存执行事实并给出 `status=failed`。
- 证据边界：单文件 64 KiB 头尾保留、省略标记、缺失文件不等于空输出、CSV 只做结构统计、
  JSON/配置脱敏、目录越界/硬链接/符号链接拒绝、scope 隔离。
- `internal.log`（运行与会话的生命周期日志）作为 `kind=log` 证据导入，缺失不算告警。
- 受限区间读取：只允许 `stdout.bin`/`stderr.bin`/`internal.log`，单次 ≤8 KiB，越界参数被拒；
  读到的片段会成为新的证据条目，因此引用它同样要过校验。
- 结构化输出：字段集合、类型、长度、每条主张 1–6 条引用、**引用原文 ≥8 字符**且必须是供给证据的子串；
  一次修复机会，仍失败则 `status=failed` 并保留 `execution`。
- 检索：BM25、受控测试向量的混合 RRF、向量模型版本与维度契约、重新导入后的缓存失效。
- 传输：HTTPS 要求、重定向拒绝、响应字节上限、错误体不泄漏、请求与 function calling 消息格式。
- 工具：未知工具、参数类型、越权路径、调用次数上限全部拒绝并记为 `denied`；语义检索失败时显式退化。
- 验证：模板必须 isolated/offline/单次；建议不执行；模板摘要变更拒绝；超时停止 launch plan **并留下
  `verification.json` 回执**；子进程环境过滤掉凭据；执行完成后回执写入库，下一次 `analyze` 能看到
  `verification_context`，且该运行的报告以 `origin=verification` 进入上下文。
- 指标：每条分析记录 `metrics`（修复次数、工具调用/拒绝、引用数、检索方式、语义退化、来源分布、
  上下文规模、耗时），`stats` 在 scope 内聚合出合格率与越权计数。

## 受控验证闭环（真机，已执行）

2026-11-06，操作者本人执行（脚本内部只对 sysctl 用 sudo）：

```bash
cd ~/sbx3 && bash tools/ai-verify-e2e.sh
```

结果：`AI VERIFY E2E PASSED`，四条必检行全部出现，`restored
kernel.apparmor_restrict_unprivileged_userns=1`。

| 观察项 | 值 |
|---|---|
| 被分析的运行 | `run-847dea0dcbb654874ed4d4c8`，`summary=nonzero_exit`、`exit 43`（真实的隔离执行） |
| 验证运行 | `run-6f7b69c71db1ca8d52708f4a`，`cli_exit=0`、`exit_code=0`、`verdict=ok` |
| 隔离与清理 | `isolated=true`；`cgroup_removed`、`descendants_empty`、`launcher_reaped` 均为真，`cleanup.errors=[]` |
| 回执 | `status=executed_not_automatically_confirmed`（验证成功也不自动升级为"假设已证实"） |
| 产物 | 验证程序在挂载的私有目录里写出了 `value`（10 字节），说明那次执行是真的发生在沙箱里 |
| 第二次追问 | 输入里出现 `verification_context`（该 run、该模板摘要），证据来源 `{current: 5, verification: 2}`，引用验证证据通过校验 |
| 聚合 | `stats`：`analyses=2`、`completed=2`、`structured_output_rate=1.0`、`tool_denied=0`、`verifications=1` |

同一天的更早一次尝试（模板还没挂 `/output`）验证程序自身以 41 失败，回执依然是
`executed_not_automatically_confirmed`——这说明状态不取决于目标成功与否，但那次不能当作
"验证通过"的记录，所以只保留为过程事实。

这次真机执行还暴露了 4 个 harness 侧的缺陷（状态目录 0755、可选参数被当函数调用、模板缺
`schema_version`、模板缺 `/output` 挂载），都已修在提交里；沙箱和 AI 逻辑本身没有发现问题。

## 真实模型（已执行，结论是"模型侧不达标"）

2026-11-06 对配置的兼容端点（`qwen-plus`）跑了 4 条真实运行（oom / timeout / success / exit 43），
共三轮，每轮 4 条、每条最多"一次格式修复"：

| 轮次 | 改动 | 通过 | 失败模式 |
|---|---|---|---|
| M1 | 原始实现 | **1/4** | 引用把紧凑 JSON 逻辑重排后输出 |
| M2 | 报告改缩进 JSON、引用允许只有空白不同、修复轮回传具体错误、HTTP 超时 30→60s | **1/4** | 1 条 30s 传输超时；2 条仍非逐字 |
| M3 | 再加"关键事实的逐行渲染"作为额外证据 | **0/4** | 4 条全部非逐字 |

**失败模式是同一个**：模型把证据里**不同位置的真实行拼成一段"引用"**，并用 `\n` 连起来，例如

```
QUOTE : "[18:22:47] … init created and resources attached, pid=131184\n[18:22:47] … init event: exec_observed\n…"
EVID  : "[18:22:47] … state=preparing\n[18:22:47] … run directory: …\n…"
```

每一行都能在证据里找到，但它们不相邻；另有一处 `timing` 块里的数字被改动（`total_ms: 48` vs 证据里的值）。
这不是引用的格式问题，是把两处事实拼成一句话——**放宽校验就等于允许拼接和捏造因果关系**，所以没有放宽。

其余观察：

- 传输与接口兼容性**已验证**：HTTP 请求、`usage` 回传、`finish_reason`、超时控制都按预期工作。
- 工具调用 0 次；越权 0 次（这几条证据足够回答，模型没有主动取证）。
- 只有 `oom` 用例曾完整通过（5 条事实、2 条假设、7 条引用、4 条 `missing_info`），说明模型偶尔能做到，
  方差很大，4 条样本也不足以给出合格率。
- 这轮也验证了**校验本身不是摆设**：模型编造/拼接的引用被逐条拦下，`status=failed` 且保留 `execution`。

想让这类模型真正可用，方向不是放宽引用规则，而是**把证据做成不可拼接的形态**：例如证据按行编号、
每行一个独立 id，引用必须"某一行"而不是一段文本——拼接在构造上就不可表达。这属于改造，未实现。


## 未验证

- **诊断质量**：`qwen-plus` 在三轮里 1/4、1/4、0/4，引用不合格；没有通过校验的样本可谈结论对错。
  换更强模型或换成"按行编号"的证据形态后可重测。
- 验证模板的其它边界（超时、模板变更拒绝、目标失败）只在契约测试里验证过；真机只跑过成功的一次。
- 真实模型面对提示注入、错误线索和复杂任务时的推断可靠性；离线测试不能证明抗注入能力。
- Linux 符号链接检查已在本机执行；对**并发**恶意替换目录的抵抗能力仍未验证。
- 引用校验只证明"这段话确实来自这份证据"，不证明推断正确；`stats` 的合格率衡量格式与引用，
  不衡量结论对错。

测试未调用云模型，也未启动真实隔离验证；模拟传输、模拟执行器和合成证据的成功不得描述为
上述未验证项目已通过。
