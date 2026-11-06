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

## 真实模型（部分执行，外部阻塞）

2026-11-06，对配置的兼容端点（`qwen-plus`）跑了 4 条真实运行（oom、timeout、success、exit 43）：

| 观察项 | 结果 |
|---|---|
| 传输与接口兼容 | 通（HTTP 请求、`usage` 回传、`finish_reason` 都正常） |
| 结构化输出合格率 | **1/4**（3 条在"修复一次"之后仍失败） |
| 工具调用 / 越权 | 0 次调用、0 次拒绝 |
| 失败原因（逐条核对） | 模型把证据里的**紧凑 JSON 片段重新排版**后再引用（如 `"summary":"oom","timing":{…}`），不是原文子串 → 引用校验正确拒绝 |

据此做了三处修复（不放宽"引用必须来自证据"的底线）：

1. 报告证据改为**缩进 JSON**（紧凑 JSON 既难被逐字引用，也不利于人读）；
2. 引用校验允许**只有空白**不同（应对 JSON 重排），其余字符必须按序出现；改词、重排内容、缩写仍被拒；
3. 修复轮把**具体校验错误**回给模型（"把重排过的 JSON 引用改回逐字"），不再只说"校验失败"。

50 项契约测试覆盖了这三条：重排 JSON 的引用通过、改写的引用仍被拒、报告证据是缩进 JSON。

**第二次测量没能完成**：修复后再次评测，服务商返回 `Access denied … Arrearage`（账号欠费），
4 条全部在第一次调用就失败。这是账号状态问题，不是代码问题；充值或换 key 后重跑
`tools/ai_real_eval.py` 即可得到修复后的合格率。因此下表里的"诊断质量"仍然没有数据。

## 未验证

- 真实服务商的**诊断质量**（接口兼容性与 `usage` 已验证；欠费导致修复后的第二次测量未完成）。
- 验证模板的其它边界（超时、模板变更拒绝、目标失败）只在契约测试里验证过；真机只跑过成功的一次。
- 真实模型面对提示注入、错误线索和复杂任务时的推断可靠性；离线测试不能证明抗注入能力。
- Linux 符号链接检查已在本机执行；对**并发**恶意替换目录的抵抗能力仍未验证。
- 引用校验只证明"这段话确实来自这份证据"，不证明推断正确；`stats` 的合格率衡量格式与引用，
  不衡量结论对错。

测试未调用云模型，也未启动真实隔离验证；模拟传输、模拟执行器和合成证据的成功不得描述为
上述未验证项目已通过。
