# 威胁模型：这个沙箱防什么、不防什么

本文只描述**当前实现**能证明的东西，每条声明后面挂着能验证它的测试。没测试支撑的写法是"未验证"。

## 1. 资产

| 资产 | 为什么重要 |
|---|---|
| 宿主机文件系统 | 不可信程序不该读到或改到它 |
| 宿主进程与 PID 视图 | 不该看见、也不该信号到宿主进程 |
| 宿主机网络 | `network: none` 时不该有出网能力 |
| 宿主机 CPU / 内存 / 磁盘 | 不能被单个任务耗尽 |
| 产物完整性 | 上层要拿它做判定，被截断/替换的产物会误导决策 |
| 其他并发任务 | 任务之间不该互相看见或干扰 |

## 2. 对手模型

**在范围内**：一个不可信的**用户态程序**，可能故意：
- 越界读写挂载点、构造路径穿越或符号链接；
- 试图 `ptrace`/发信号给宿主进程、读宿主的 `/proc`；
- 疯狂 fork、耗尽内存/磁盘/inode、无视 SIGTERM；
- 通过继承的 fd 或环境变量拿到不该拿的能力；
- 写伪造成"成功"的退出码，同时让产物缺失或截断。

**不在范围内**：
- 内核漏洞利用（本地提权、namespace 逃逸的 0day）——本沙箱以**内核为信任基**；
- 侧信道（缓存计时、Spectre 类）；
- 物理/硬件攻击与宿主机 root 本身的滥用；
- `network: host` 或 `process` 模式下的隔离（明确不是安全边界）；
- 多租户鉴权（单用户本地工具，见 README 边界）。

## 3. 在范围内的防护与对应证据

| 声明 | 实现位置 | 证据 |
|---|---|---|
| 目标看不到宿主 `/proc`、看不到宿主 PID | 新 PID namespace + init 作为 PID 1 + 重新挂载 `/proc` | `tests/acceptance/isolation.py`（`proc-view`：比对 `/proc/self` 与自身 PID、确认 `/.oldroot` 不可达） |
| `network: none` 时没有可用网络 | 新 net namespace，只有 loopback | 同上（`network` 用例） |
| 只读挂载不能写、写挂载不能越界 | 挂载前解析目标、`MS_RDONLY`、`pivot_root` 后不保留旧根 | `tests/acceptance/isolation.py`、`tests/integration/mount_source_tests.cpp` |
| 符号链接不能作为证据/输入被跟随 | 读取器 `O_NOFOLLOW` + `lstat` 逐段检查 | `tests/unit/ai/test_analysis.py`（link/hardlink 拒绝） |
| 硬链接不能绕过文件读取限制 | `st_nlink == 1` 校验 | 同上（`test_hardlink_is_rejected`） |
| 资源被硬上限卡住（CPU/内存/pids/磁盘） | cgroup v2 `cpu/memory/pids` + tmpfs `size`/`nr_inodes` | `tests/acceptance/isolation.py`（over-quota → ENOSPC 60）、`workspace_quota.py`（byte/inode 配额） |
| 无视 SIGTERM 的目标会被强杀 | `grace_ms` 后 SIGKILL，回执如实记 `timeout` | `tests/acceptance/isolation.py`、`tools/ai_verify_e2e.py`（`ignore-term` 用例） |
| 目标死后不留孤儿进程 | cgroup 内 `kill_all` + `descendants_empty` 校验 | `tests/acceptance/isolation.py`、`cleanup` 字段断言 |
| 产物未交付时**不报成功** | `workspace.export.ok=false` → 判定失败并说明原因 | `tests/acceptance/isolation.py`（"a refused export is reported, not passed off as success"） |
| 产物原子替换、不会半新半旧 | `renameat2(RENAME_EXCHANGE)` + 父目录 `fsync`，跨文件系统时 copy-then-exchange | `tests/integration/publish_tests.cpp` |
| 配额被打满时仍交付已写部分 | 导出在停止前完成，`workspace` 字段记录字节/条目 | `tests/acceptance/isolation.py`（over-quota 用例） |
| 运行结束后证据被篡改 | 一次性运行的目录**和会话命令目录**都写 `checksums.json`（含 `result.json`/`config.json`/`request.json`/`stdout.bin`/`stderr.bin`/`internal.log` 中存在的那些）；AI 层导入时逐一校验，不匹配即拒绝 | `tests/integration/integration.py`（校验和与磁盘一致）、`tests/unit/ai/test_analysis.py`（篡改即拒收、缺文件记 partial、无校验和记 unverified） |
| 控制通道的 fd 不泄漏给目标 | init 只把 stdin/stdout/stderr 传下去，其余 `CLOEXEC` | `tests/integration/control_fd_tests.cpp`、`tests/integration/session_runtime_tests.cpp` |
| 会话关闭期间的通道异常不会被误报成成功 | 关闭后迟到回执按序号丢弃、通道作废 | `tests/integration/sessions.py`、`tests/unit/ai`（宿主侧不依赖通道文本） |

## 4. 被发现过的真实问题（说明这套东西不是想出来的）

| 现象 | 根因 | 回归测试 |
|---|---|---|
| 一次运行报"成功"，但产物没交出来 | init 退出时未读 `STOP`，对端 `ECONNRESET` 丢掉已发的 `INIT_DONE` | `tests/integration/publish_tests.cpp`、`tests/integration/integration.py` |
| 会话关闭被误报成 `host_error` | 同上，只是发生在 `close()` 路径 | `tests/integration/session_runtime_tests.cpp`、`tests/integration/sessions.py` |
| 传输超时后通道复用 | 迟到回执被当成协议错误 | `tests/integration/session_wire_tests.cpp`、`tests/integration/sessions.py` |

这些不是"设计里考虑过"，是**真机上抓到并修掉**的；它们恰好构成"为什么需要这套语义"的最好论据。

## 5. 明确不提供的

- **不是多租户**：`scope` 是单用户工具内的项目边界，不是鉴权。
- **不是 SELinux/AppArmor 加强**：安全策略是 `baseline` seccomp，不阻止所有危险 syscall。
- **不防内核与硬件层攻击**：这是设计前提，不是遗漏。
- **`process` 模式没有隔离**：它只用于本机诊断，需要 `--allow-unsafe` 显式开启；报告里 `isolated=false`。
- **不保证跨重启的产物留存**：工作区是 tmpfs，导出之后由调用方保管。

## 6. 已知薄弱点（未验证或未做）

1. **并发/迁移**：存储层是单进程假设（`flock` 与 `meta` 版本检查在本轮补上，见 `docs/CONTRACT.md`）；没有多写入者压测。
2. **侧信道与共享内核的时间攻击**：未评估。
3. **挂载源的并发替换**：测试覆盖一次快照，不覆盖"读的时候目录被换掉"的竞态。
4. **显存/GPU**：完全不涉及。
5. **审计日志的防篡改**：`internal.log` 是普通文件，没有签名链。

任何一条被问到，正确回答是"已知、未验证、范围外"，而不是"应该没问题"。
