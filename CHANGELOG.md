# 变更记录

版本号来自 `CMakeLists.txt` 的 `project(... VERSION)`；`sandbox --version` 打的是同一个值。

## 0.5.0

沙箱本体

- 隔离执行：user / mount / pid / net namespaces、PID 1 init、seccomp 基线策略、cgroup v2 限额
  （cpu / memory / pids / tmpfs 字节与 inode 配额）。
- 判定语义：`verdict` 与 `summary` 分离；**产物未交付绝不算成功**；清理失败如实上报（`125`）。
- 产物交付：staging + `renameat2(RENAME_EXCHANGE)` 原子发布，跨文件系统 copy-then-exchange，
  `durable` / `retained_previous_path` 单独成事实。
- 持久会话与批处理；Python SDK（`sdk/`）与 `sandbox serve_session` 控制通道（protobuf 消息定义在
  `proto/session_v1.proto`）。
- 运行目录自证：`checksums.json`（`result.json`、`config.json`/`request.json`、`stdout.bin`、
  `stderr.bin`、`internal.log` 的 sha256），一次性运行与会话命令都写。

分析层（`sandbox_ai`，独立 Python 包，零第三方依赖）

- 证据提取：报告、配置、stdout/stderr、`internal.log`、挂载目录；行边界对齐分块、不重叠。
- 引用契约：证据带可见行号，模型只回 `{id, line}`，原文由程序解析；模型无法伪造引用文本。
- 检索：BM25（可选向量 + RRF）+ 可选 rerank；被问题点名的文件强制进上下文；统计量按语料代际缓存。
- 工具循环：`read_evidence` / `read_output` / `search_history` / `propose_verification`，有界调用。
- 假设状态：可选 `hypothesis_updates`（supported/refuted/unknown），"支持/反驳"必须引用验证运行的证据，写库可见。
- 受控验证：操作者登记的模板（digest 锁定、`${run_dir}` 占位符、只替换白名单），回执一律
  `executed_not_automatically_confirmed`。
- 传输：429/5xx/超时按 `Retry-After` 有界重试；慢调用有心跳；`--self-check` 逐条记录引用支持度（仅记录）。
- 导入完整性：校验 `checksums.json`，不匹配拒绝导入，缺文件记 `partial`，无校验和记 `unverified`。

命令行

- `sandbox run -- [options] -- COMMAND...` 一行跑；`--summary` 六行人话；`--quiet` 关 stderr 日志。
- `sandbox help <cmd>` / `<cmd> --help`；`sandbox prune [--keep N] [--older-than 7d] [--dry-run]`。
- `sandbox doctor` 报告可用启动模式；`sandbox inspect` / `cancel` / `cleanup`；`sandbox prune` 清理已完成运行。
- GitHub composite action（`action.yml`）：在 runner 的 systemd 委派单元里跑一次隔离执行，按 `verdict` 决定成败，报告写进 job summary（见 `docs/CI.md`）。
- `sandbox-ai`（`pip install .` 或 `tools/install.sh` 后可用）：`import` / `analyze` / `explain` /
  `verify` / `stats` / `demo`。

文档

- `docs/THREAT_MODEL.md`（防什么、不防什么、每条声明对应的测试）、`docs/CONTRACT.md`（字段分级与语义承诺）、
  `docs/INSTALL.md`、`docs/RUNTIME.md`、`docs/QUICKSTART.md`、`docs/VALUE.md`（解释是否帮到下一次尝试）、
  `docs/AI_VALIDATION.md`（已执行/未执行的验证记录）、`docs/RETROSPECTIVE.md`（踩过的坑）。

## 0.4 / 0.3（历史）

固定路径安装与普通用户运行、会话与批处理、workspace 配额与会话累计导出、控制通道 fd 化移交、
报告 schema 2（`workspace` 字段）。更早的记录见 `docs/HANDOFF.md`、`docs/REDESIGN.md`。

## 未发布

- 报告 JSON Schema 与 golden 快照、CI 中运行分析层测试、预编译发行包与容器镜像。
- `pip install .` 的 wheel 构建本身未在目标环境实测（只有元数据与入口点被验证）。
