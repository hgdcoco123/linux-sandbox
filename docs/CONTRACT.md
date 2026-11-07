# 契约：什么稳定、什么会变、怎么升级

调用方（CI、Agent、脚本）需要知道哪些字段可以依赖。按"改动代价"分三级。

## 1. `sandbox run` 请求（config.json）

- **未知字段直接拒绝**（`src/model/config.cpp` 对每个对象做白名单，遇到未知键抛 `unknown field`），所以拼错字段不会静默变成默认值——这是接口能当契约用的前提。
- **稳定**：`schema_version`、`mode`、`network`、`argv`、`mounts[]`、`limits{wall_ms,startup_ms,grace_ms,memory_bytes,pids,capture_bytes,tmp_bytes}`、`workspace{max_bytes,max_entries,keep_on_close}`、`cwd`、`env`、`stdin_file`、`runtime_root`、`cgroup_root`、`session{enabled}`。
- **实验**：`metrics{interval_ms,history_limit}`、`batch` 任务数组的结构细节。
- 请求的 `schema_version` 必须是 `1`；升级规则：**新增可选字段不升版本，改变已有字段含义必须升版本**。

## 2. `sandbox run` 响应（report.json，`schema_version: 2`）

| 级别 | 字段 | 说明 |
|---|---|---|
| **稳定** | `run_id`、`summary`、`verdict`、`exit_code`、`signal`、`isolated`、`errors[]`、`exec_observed`、`exec_error` | 上层可以用它们做判定。`verdict=harness_error` 时 `exit_code` 可能是 `null`，这不是缺陷而是事实 |
| **稳定** | `cleanup{cgroup_removed,descendants_empty,launcher_reaped,errors[]}` | 清理是否完成 |
| **稳定** | `workspace{export{attempted,ok,durable,error,bytes,entries,manifest,manifest_entries,manifest_truncated,manifest_error}, mode, limits}` | **`export.ok=false` 时不得视为成功**；`manifest` 指向运行目录下的 `artifact-manifest.json`（`path/type/size/sha256`，受 2048 条与 16 MiB 哈希预算限制，`manifest_truncated` 说明是否截断） |
| **稳定** | `stop_reason`、`stdout/stderr{saved_bytes,discarded_bytes,eof,flushed,write_error}` | 输出是否被截断/丢弃 |
| **稳定** | 运行目录里的 `checksums.json`：`{schema_version, run_id, algorithm:"sha256", files:{name:{sha256,size}}}`；一次性运行覆盖 `result.json`/`config.json`/`stdout.bin`/`stderr.bin`/`internal.log`，会话命令目录覆盖 `result.json`/`request.json`/`stdout.bin`/`stderr.bin` | 下游可自证"我引用的字节就是这次运行写下的字节"；AI 层导入时校验，不匹配即拒绝。它是**自证的**：防误改，不防伪造（见 THREAT_MODEL） |
| **实验** | `metrics.*`（`cpu.stat`、`memory.*`、`pids.*`、`sampling.history`） | 字段会随内核版本变化；不要据此做硬判定 |
| **诊断** | `internal.log` 的文本格式、`security{policy,version}` | 供人读；解析请以 report 字段为准 |

**稳定级字段的语义承诺**（这些是测试锁定的，不是文档口径）：

1. `verdict=ok` ⟺ 目标正常退出**且**产物已交付；
2. 产物未交付 ⇒ `verdict != ok`，且 `errors[]` 给出原因；
3. `cleanup.errors` 非空 ⇒ 报告如实反映，不当作成功；
4. 同一 `run_id` 的报告只写一次，写到 `<state>/<run_id>/result.json`。

证据：`tests/integration/publish_tests.cpp`（原子替换/三态结果）、`tests/acceptance/isolation.py`（导出被拒不算成功、配额打满仍交付部分）、`tests/integration/integration.py`（判定与退出码）。

## 3. AI 层：`sandbox_ai` 的接口

| 对象 | 稳定 | 实验 |
|---|---|---|
| 证据 `Bundle` | `schema_version`（当前 2）、`scope`、`run_id`、`evidence[{id,scope,run_id,kind,source,location,text,truncated}]`、`warnings[]` | `run_dir`（区间读取用，v1 快照没有） |
| 分析结果 | `analysis_id`、`run_id`、`status`、`analysis{summary,facts[],hypotheses[],missing_info[],next_steps[]}`、`resolved_citations[]`、`metrics`、`warnings[]` | `analysis.hypothesis_updates[]`（可选字段）、`metrics` 的具体键 |
| 引用 | `{id, line}`，原文由程序解析为 `resolved_citations[].text` | 可选的 `quote` 交叉检查字段 |
| 验证回执 | `status=executed_not_automatically_confirmed`、`run_id`、`template_digest`、`execution`、`hypothesis` | 其它字段 |

**语义承诺**：`status=completed` 表示回答通过 schema 与引用校验；它**不表示**结论正确。

## 4. 存储（SQLite）

- `PRAGMA user_version` = 当前 schema 版本（`SCHEMA_VERSION = 1`）。
  - 库版本 **高于**代码 → **拒绝打开**（`This database was written by a newer version`），不猜测迁移；
  - 库版本低于代码 → 就地升级（当前只有版本记录，无破坏性迁移）。
- **单写入者**：数据库旁的 `<db>.lock` 用 `flock` 排他，第二个进程打开会立即报错，而不是写出重复行。
  - 非 POSIX 平台没有该锁（Windows 上退化为无保护），单用户本地工具可接受，但在并发环境下必须知道这一点。
- WAL 模式：读不阻塞写；跨进程并发**写**是不支持的。

## 5. 升级与回滚

1. 升级代码后直接运行：库版本不变则无需操作；库版本更高则先升级工具，或换一个 `--db` 文件。
2. 回滚代码：旧代码打开新版库会被拒绝（安全方向），把库文件一起回滚。
3. 缓存：分析缓存键包含 `PROMPT_VERSION`、证据指纹、语料代际、模板目录与预算；升级代码后旧缓存不会命中，无需手工清理。

## 6. 明确不在契约内

- `internal.log` 的行格式、日志文件名之外的一切；
- `metrics` 里任何具体数值（随内核/采样间隔变化）；
- AI 层提示词的具体措辞（`PROMPT_VERSION` 一变就会变）；
- 临时目录布局（`<state>/<run_id>` 之外）。
