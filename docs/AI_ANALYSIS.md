# 通用 AI 分析层：操作、架构与验证边界

## 1. 实现了什么

本层面向运行记录和产物，不依赖题目、学生或答案字段。支持：

- 完成的单次执行 RunReport、持续会话单条命令 core record 的证据适配。
- 运行事实、stdout/stderr、`internal.log` 生命周期日志、源码及文本/JSON/CSV产物快照；
  来源位置和内容摘要。
- 结构化生成，事实与假设分开；引用是**行地址**（`{"id": "ev-…", "line": N}`），原文由程序解析，
  模型没有"写引用文本"的通道；一次格式修复机会。
- BM25关键词检索；可选真实embedding、余弦检索、RRF融合；可选 **rerank 精排**（召回→精排两阶段），
  精排不可用显式退化；每条证据带 `origin`（current / history / verification），历史运行不会被当成本次证据。
- 原生function calling：读取证据、**按区间重读被捕获的输出流**、搜索其他运行、提出验证模板。
- 有界多轮上下文、内容和模型版本相关的缓存、审计记录，以及每条分析的 `metrics`
  （含 `resolved_citations`：模型指到的那一行到底是什么）。
- 显式执行已登记的隔离验证模板，导入新证据并**把这次验证记入库**，再在原对话追问时可见。
- **假设状态可闭合**：回答可选带 `hypothesis_updates`（supported / refuted / unknown）；
  "支持"或"反驳"必须引用**验证运行**的证据，记录进 `hypothesis_status`，后续轮次可见。
- **验证模板可用 `${run_dir}`** 引用本次运行的产物目录，由程序替换（不接受模型给的路径）。
- **传输鲁棒性**：429/5xx/超时按 `Retry-After` 有界重试；慢模型调用期间打印心跳（`--quiet` 可关）。
- `explain <run-dir>`：一条命令、一次调用，打印"发生了什么 / 已确认 / 假设 / 未知 / 建议"，
  不建立会话；`stats` 把 scope 内的分析聚合成合格率、引用、工具越权、失败原因与假设状态。

不引入Web服务、自动任意命令执行或多Agent调度。向量使用SQLite存储和精确扫描，
项目范围上限10000块；这是单机原型，不宣称达到大规模向量服务的性能。

## 2. 先运行离线演示

在仓库根目录执行（Windows PowerShell和Linux均可）：

```text
python -m sandbox_ai --db ai-demo.sqlite3 --scope demo demo --output ai-demo-evidence
```

output目录必须不存在。生成三类**合成**执行记录：C++链接错误、CSV结构问题、超时。
确定性DemoProvider会展示真实的工具路由、引用校验和多轮存储，但不产生真实AI诊断。
三份analysis文件都明确标记offline-demo。它不调用云模型，也不启动沙箱。

不要把此输出作为Linux隔离或真实模型效果验证。再次运行换一个输出目录。

运行本地测试：

```text
python -m unittest discover -s tests/unit/ai -v
```

## 3. 导入真实运行

先用现有CLI/SDK运行任务，等到result.json已生成、产物导出结束。
导入目录是**包含result.json、stdout.bin、stderr.bin的目录**，不是state-root父目录。

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo import \
  --run-dir /tmp/sandbox-state/run-REPLACE \
  --sources /path/to/relevant-source \
  --artifacts /path/to/exported-artifacts \
  --context '任务要求：编译生成可执行文件，并解释失败原因。'
```

sources和artifacts都是可选的，由操作者显式指定。不要指定包含密钥或无关文件的整个主目录。
同scope/run_id重新导入会替换证据，并让旧的向量失效。上下文是业务要求，不是已确认事实。
导入会返回run_id；持续会话命令ID由会话目录和命令index共同组成，后续使用返回值。

证据提取每文件最多64KiB，超限日志保留头尾；每个源码/产物根最多32个文件、128个目录，
总共最多256块。超限、缺失和不支持的格式会记录warnings。CSV只做结构统计，
不会凭空判定业务正确性。JSON可能经过脱敏和格式化，引用指向规范化证据片段。

`internal.log`（运行与会话共用的生命周期日志）也会作为 `kind=log` 证据导入。它记录控制事件的
顺序、通道何时关闭、工作区导出是否成功，是报告里看不到的那部分事实。旧运行没有这个文件，
缺失不算告警。

模型还能用 `read_output(stream, offset, length)` 按区间重读 `stdout.bin`/`stderr.bin`/`internal.log`：
单次最多8 KiB，只能是这三个流，读到的片段会成为新的证据条目——引用它同样要过引用校验。

## 4. 接入真实模型

通过兼容的Chat Completions和Embeddings HTTP接口接入，例如百炼提供的兼容接口。
服务地址和模型名称必须使用当前账号、区域实际支持的值；不在代码里写死模型。

Linux环境变量：

```bash
export SANDBOX_AI_BASE_URL='https://YOUR-ENDPOINT/compatible-mode/v1'
export SANDBOX_AI_MODEL='YOUR-CHAT-MODEL'
export SANDBOX_AI_EMBEDDING_MODEL='YOUR-EMBEDDING-MODEL'
read -rsp 'API key: ' SANDBOX_AI_API_KEY
export SANDBOX_AI_API_KEY
```

PowerShell可通过`$env:SANDBOX_AI_BASE_URL`等设置配置；凭据从本地环境注入，不写入仓库。
运行compatible分析或semantic索引会把选中的证据发送给配置的服务商。脱敏是尽力而为，
不是完整敏感数据识别，调用方需先筛选上传范围。默认验证TLS证书，拒绝HTTP重定向。
本地模型可显式`--allow-local-http`，只允许localhost、127.0.0.1或::1。

官方接口参考：
- https://help.aliyun.com/en/model-studio/qwen-api-via-openai-chat-completions
- https://www.alibabacloud.com/help/en/model-studio/text-embedding-synchronous-api

先不启用语义检索，分析一条运行：

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo analyze \
  --run-id run-REPLACE --question '本次任务为什么失败？请区分事实、推断和待验证内容。'
```

要用真实向量：

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo --semantic index
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo --semantic search '未定义符号 链接失败'
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo --semantic analyze \
  --run-id run-REPLACE --question '是否有类似的历史失败？它们能否解释本次问题？'
```

embedding模型变更或重新导入后重新index。向量失败时分析会明确标注退化为关键词检索，
不会伪装语义检索成功；search命令则直接报错。index逐块调用模型，会产生调用成本。
目前没有自动网络重试，避免隐式重复请求；模型格式修复最多一次。

输出的execution来自导入报告，analysis来自模型。status=failed仍保留execution。

引用是**行地址**而不是文本：供给模型的每条证据形如 `{"id": "ev-…", "lines": ["…", "…"]}`，
模型只能回 `{"id": "ev-…", "line": 12}`（1 基）。程序按行号取出原文写进
`result["resolved_citations"]`，所以：

- **伪造没有通道**：模型不生产引用文本，它只能指向一个已存在的行；指错行是"选错证据"，不是"编造证据"。
- 行号越界、指到不足 8 字符的行（如 `}`）、指向未供给的证据，都会被拒。
- 可选的 `quote` 字段只作交叉检查：对不上就拒，但它不能引入新文本。
- 仍然只证明"这行原文存在"，不证明推断正确。

usage记录服务商实际返回的用量，未返回时不估造token数。

证据给模型时每行带 `L12| ` 前缀，行号是可见标签、不需要数；引用只回行号。`stats` 另给出 `failure_reasons`（transport / citation / schema / budget）分布。检索是两阶段：先 BM25（可选、加向量后）RRF 融合取回候选，若启用 `--rerank`
（`SANDBOX_AI_RERANK_MODEL`）再调 `/rerank` 精排；精排失败**降级保留融合顺序**并写进 `warnings`，
`metrics.reranked` 为 false，`metrics.retrieval` 标记为不含 `+rerank`。

每条分析还记录 `metrics`，把五项能力变成可核对的数字：

```json
{"repairs":0,"tool_calls":1,"tool_denied":0,"citations":4,"retrieval":"lexical_bm25",
 "semantic_degraded":false,"origins":{"current":6,"history":1,"verification":0},
 "evidence_seen":7,"context_chars":8123,"calls":2,"usage_records":2,
 "elapsed_seconds":1.2,"cache_hit":false}
```

整个scope聚合成一组合格率：

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo stats
```

`structured_output_rate` 是"通过校验的分析 / 分析总数"，`tool_denied` 是越权尝试次数（应为0），
`semantic_degraded` 是伪造语义检索成功的次数（应为0）。它衡量格式与引用，不衡量结论对错。

## 5. 多轮追问

第一次analyze返回session_id，用它继续，可以切换到同项目内另一条运行：

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo analyze \
  --run-id run-OTHER --session chat-REPLACE \
  --question '与上一条运行相比有什么变化？哪些原因仍未验证？'
```

保留最近三轮摘要、问题和运行ID；历史AI结论仅是上下文，不自动升级为证据。
聊天上下文不会隐式创建或复用沙箱持续会话。模型最多调用6轮、4次工具，默认上下文
48000字符上限（不是token计数），180秒分析期限；单次HTTP默认90秒、回答预算默认3000 token
（两者都是参数，上限180秒/8000 token，越界直接报错而不是悄悄截断）。
期限检查在HTTP调用边界进行，不能中断正在进行的同步请求。

上下文里每条证据都带 `origin`：`current` 是本次运行，`history` 是其他运行，
`verification` 是操作者为早先提案执行过的验证运行。只有 `current` 在讲本次执行，
历史运行最多算类比。

## 6. 受控验证闭环（Linux）

操作者预先准备一个可信的isolated配置，例如只打印某个依赖版本。配置必须：
- 声明 `schema_version: 1`（沙箱本身也会拒；登记时先查，避免执行到一半才发现）。
- mode=isolated，network=none，非持续会话；argv、挂载和输入由操作者固定。
- 显式给出wall_ms<=30000、startup_ms<=15000、grace_ms<=2000、
  memory_bytes<=1GiB、pids<=128、capture_bytes<=1MiB、tmp_bytes<=128MiB。
- 满足现有沙箱自身的配置校验。原始模板不能来自模型或被分析产物。

例如创建templates.json（路径相对该文件）：

```json
{
  "check_dependency": {
    "description": "使用预先准备的程序检查运行时依赖是否可用",
    "config": "check-dependency.json"
  }
}
```

在analyze中添加`--templates /path/to/templates.json`。模型只能选择登记的模板，
不能提供任意argv、路径、资源限制或网络设置。propose_verification仅生成建议。

操作者检查建议后显式执行：

```bash
python3 -m sandbox_ai --db /tmp/analysis.sqlite3 --scope build-demo verify \
  --analysis-id analysis-REPLACE --proposal-id proposal-REPLACE \
  --templates /path/to/templates.json \
  --binary /opt/linux-sandbox/bin/sandbox --results /tmp/ai-verify-01
```

不加sudo。复用sdk.launch.plan_launch的普通用户systemd委派/现有cgroup流程。
配置摘要不匹配时拒绝；无Linux不退回process；外部等待超时停止对应launch plan，并留下
一份 `verification.json`（`status=timed_out`），不会因为超时就把现场删掉。
子进程只继承过滤后的环境：`SANDBOX_AI_API_KEY` 以及任何看起来像凭据的变量都不会传进去
（systemd --user 需要的会话变量保留）。
模板摘要锁定配置文本与解析后的路径，不锁定运行时二进制或挂载目录内容，操作者仍需维护其可信性。
验证返回目标失败也会作为新证据导入，不能把“执行过”解释成“假设已证实”。

收到新run_id后，在同session再次analyze，询问验证是否支持原假设。`verification.json`
记录原analysis/proposal、模板摘要和新run_id，形成可追溯链路；同一份回执也写进数据库，
所以下一次 `analyze` 的输入里会出现 `verification_context`（哪个提案、哪个模板、哪条运行、
`executed_not_automatically_confirmed`），该运行的报告以 `origin=verification` 进入上下文，
模型可以引用它、也必须把它当“关于提案的证据”而不是“关于本次运行的证据”。

### 一次跑完整条链路

```bash
# 由操作者本人运行（脚本内部自己调 sudo），不要加 sudo
bash tools/ai-verify-e2e.sh
```

它按 `tools/isolated-accept.sh` 同样的方式放行/恢复 host 策略，然后在委派单元里演完整条链路：
执行一次真实隔离运行 → 导入 → 脚本化模型提出登记模板 → 真实执行 `verify`（隔离、离线、有界）
→ 断言回执入库且状态仍是 `executed_not_automatically_confirmed` → 下一次追问能看到
`verification_context` 且能引用验证运行（`origin=verification`）的证据 → 打印 `stats`。
`--dry-run` 只校验配置、模板登记和摘要，不碰沙箱也不改 host 策略。

## 7. 测试与目前的边界

78 项契约测试已注册进 CTest（`ai_contracts`），另有 `tools/ai-check.sh` 串起测试、离线 demo
和 `stats` 断言。测试覆盖证据截断/缺失、`internal.log` 导入、区间读取边界、CSV结构、脱敏、
路径/链接拒绝、scope、缓存失效、向量模型版本、真实HTTP请求格式的模拟传输、工具越权、
引用过短与错误引用修复、模型故障、调用预算、多轮上下文、`origin` 标记、验证结果的可见性、
模板变更、验证超时回执与子进程环境过滤。

尚需真实环境验证：
1. 模型服务商的生成、embedding、tool calls兼容性及实际诊断质量。
2. Linux沙箱验证闭环的真实执行（namespace/cgroup/systemd 委派、清理与产物回收）。
3. 已知原因样例的人工评审：原因是否正确、引用是否支持结论、验证步骤是否有用。

离线测试不证明模型抗提示注入能力；工具权限与调用预算是确定性约束。
读取器拒绝链接，检查文件读取期间变化，但不是对并发恶意目录替换的完整防御。
只导入已经完成且不再被修改的导出快照。数据库是同一操作者的本地数据，scope
不是网络授权机制。未实现图片理解、PDF/DOCX解析、自动代码修改或自主执行循环。

## 8. Python调用

```python
from sandbox_ai.evidence import collect
from sandbox_ai.store import Store
from sandbox_ai.providers import CompatibleProvider
from sandbox_ai.service import Analyzer

store = Store('/tmp/analysis.sqlite3')
try:
    bundle = collect('project', '/path/to/completed/run', context='任务要求……')
    store.put_bundle(bundle)
    provider = CompatibleProvider('https://YOUR-ENDPOINT/v1', 'YOUR-MODEL',
                                  embedding_model='YOUR-EMBEDDING',
                                  rerank_model='YOUR-RERANK')      # 可选，两阶段检索
    result = Analyzer(store, provider, semantic=True, reranker=provider).analyze(
        'project', bundle.run_id, '失败原因是什么？')
    for item in result['resolved_citations']:                     # 程序解析出的真实原文
        print(item['id'], 'L' + str(item['line']), item['text'])
    print(result['metrics'])                                       # 含 retrieval / reranked / origins
finally:
    store.close()
```

`result['analysis']['hypothesis_updates']` 存在时表示这一轮把先前假设判成了
supported / refuted / unknown；它已被写入 `hypothesis_status`，下一轮会作为
`known_hypotheses` 出现在提示里。
