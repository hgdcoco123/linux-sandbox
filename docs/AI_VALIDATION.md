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

## 未验证

- 真实服务商的生成、embedding、工具调用兼容性、用量统计和**诊断质量**。
- Linux 上真实执行验证模板（namespace/cgroup/systemd 委派）以及该程序的清理与产物回收；
  执行器目前只用假 launch plan 与假子进程验证调用契约。
- 真实模型面对提示注入、错误线索和复杂任务时的推断可靠性；离线测试不能证明抗注入能力。
- Linux 符号链接检查已在本机执行；对**并发**恶意替换目录的抵抗能力仍未验证。
- 引用校验只证明"这段话确实来自这份证据"，不证明推断正确；`stats` 的合格率衡量格式与引用，
  不衡量结论对错。

测试未调用云模型，也未启动真实隔离验证；模拟传输、模拟执行器和合成证据的成功不得描述为
上述未验证项目已通过。
