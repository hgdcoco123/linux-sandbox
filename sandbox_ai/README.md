# 通用执行证据分析层

独立于 C++ 执行核心的 Python 3.10+ 库和 CLI，无新增第三方 Python 依赖。
不是 Python 判题器，也不修改原始报告或把模型输出当成沙箱执行事实。

入口：`python -m sandbox_ai --help`。完整操作见 `docs/AI_ANALYSIS.md`。

| 模块 | 职责 |
|---|---|
| models.py | 带版本的 Bundle / Evidence（记录来源目录）、内容摘要 |
| evidence.py | 单次运行和会话命令适配，日志头尾、`internal.log`、有界文本/JSON/CSV提取、区间读取 |
| store.py | SQLite快照、BM25、向量余弦与RRF融合、缓存、对话历史、验证回执、**假设状态**、语料代际缓存 |
| providers.py | 兼容Chat Completions的原生function calling、embedding、可配置路径的 rerank、有界重试与心跳；离线演示提供者 |
| service.py | RAG上下文、两阶段检索、工具循环、结构化校验、**行地址引用解析**、**假设状态**、`origin`标记、审计与`metrics` |
| verification.py | 注册模板、验证建议、显式Linux执行（过滤环境）、新证据导入与回执入库 |
| cli.py | 导入（幂等，`--reimport` 强制）、索引、检索、分析（`--self-check`）、验证、`stats`、**`explain`**、离线演示 |

本地数据库保存已导入证据，不要把它当公开文件。scope 是单用户工具内的项目边界，
不是多租户鉴权。接入网络服务时由认证层绑定 scope，不能信任用户任意填写的 scope。

不支持直接解析 PDF、DOCX、图片内容；这些文件目前只保存元信息。
后续复杂提取器应在沙箱里运行，不能在分析宿主上直接执行未知产物。
