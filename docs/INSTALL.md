# 安装与启动模式

## 1. 前置条件

| 项 | 要求 | 不满足会怎样 |
|---|---|---|
| 操作系统 | Linux，已启用 cgroup v2（`stat -fc %T /sys/fs/cgroup` 显示 `cgroup2fs`） | `isolated` 不可用，只能用 `process` 模式做诊断 |
| 内核 | 支持 user namespace；Ubuntu 23.10+ 还需放宽 `kernel.apparmor_restrict_unprivileged_userns`（见下） | `isolated` 启动失败，`doctor` 会给出 stage |
| 用户会话 | 有 user systemd（`systemctl --user is-system-running`）或显式提供 `cgroup_root` | 无法委派 cgroup，`isolated` 不可用 |
| 权限 | **不需要 root**；反之以 root 运行时委派路径会被拒绝 | — |
| 构建产物 | `build/sandbox`、`build/sandbox-launcher` | 安装脚本报错 |

## 2. 安装

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
bash tools/install.sh                    # 默认 ~/.local
bash tools/install.sh --prefix /opt/linux-sandbox   # 需要写权限，仍然不需要 root 运行
```

安装内容（全部在 prefix 内，不启用任何服务）：

```
$prefix/bin/sandbox                    # CLI（会在同目录找 sandbox-launcher）
$prefix/bin/sandbox-launcher
$prefix/share/linux-sandbox/sdk/*.py   # Python SDK
$prefix/share/linux-sandbox/tools/*.py # 委派/运行时辅助脚本
```

## 3. 三种启动模式

判定顺序：**systemd 用户委派 → 显式 `cgroup_root` → `process` 诊断模式**。

| 模式 | 何时可用 | 怎么用 | 隔离性 |
|---|---|---|---|
| **委派（默认）** | 有 user systemd、非 root | `sandbox run --config x.json`（自动 `systemd-run --user`） | 完整：user/mount/pid/net namespace + cgroup v2 限额 |
| **显式 cgroup_root** | 没有 user systemd，但能写某个被委派的 cgroup 子树 | 配置里写 `"cgroup_root": "/sys/fs/cgroup/.../runs"` | 同上（限额来自该子树） |
| **process 诊断** | 只想在本机复现执行/采集输出，不需要隔离 | `sandbox run --config x.json --allow-unsafe`（配置 `"mode":"process"`） | **无隔离**，报告里 `isolated=false` |

`process` 模式不是安全边界，只用于"本机诊断"；它也会写 `result.json`、`internal.log`、`stdout/stderr.bin`，所以下游分析可以复用。

## 4. 环境自检

```bash
sandbox doctor
```

输出里的 `isolation_probe` 说明当前能到哪一步：

| `probe.stage` / `error` | 含义 | 处理 |
|---|---|---|
| `available: true` | `isolated` 可用 | — |
| `stage: userns` / `make_mounts_private` + `Permission denied` | 不允许非特权 user namespace | Ubuntu 23.10+：`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`（**临时**，用完恢复；`tools/isolated-accept.sh` 就是保存/恢复的做法） |
| `stage: cgroup` | 没有委派的 cgroup 子树 | 在 user systemd 会话里运行，或显式配置 `cgroup_root` |
| 其它 `stage`（`make_mounts_private`、`mount_target`、`pivot_root`…） | 该步骤失败，`stage` 就是失败的那一步 | 按 `error` 里的 errno 处理；挂载相关多半是源目录权限或目标已存在 |
| `error: …` | 具体内核返回 | 按错误信息处理；`doctor` 的完整输出里有 `errno` |

## 5. 第一次运行

```bash
mkdir -p /tmp/input /tmp/output
printf 'hello\n' > /tmp/input/sample.txt
cat > /tmp/job.json <<'JSON'
{"schema_version": 1, "mode": "isolated", "network": "none",
 "argv": ["/usr/bin/sandbox-fixture", "files"],
 "mounts": [{"source": "/tmp/input", "target": "/input", "readonly": true},
            {"source": "/tmp/output", "target": "/output", "readonly": false}],
 "limits": {"wall_ms": 2000, "startup_ms": 5000, "grace_ms": 200}}
JSON
sandbox run --config /tmp/job.json
```

报告写到 `~/.local/state/linux-sandbox/<run_id>/`（可用 `--state-root` 改）。`verdict=ok` 才表示"目标正常退出且产物已交付"；字段分级见 [CONTRACT.md](CONTRACT.md)。

## 6. 装完能做什么、不能做什么

- 能：单机跑不可信程序、拿结构化报告与产物、跑 Python SDK / 会话 / 批处理。
- 不能：多租户、鉴权、远程调用；没有 systemd 委派又没有 `cgroup_root` 时无法做 `isolated`。
- `process` 模式与 `network: host` 都**不是**安全边界，报告里会标 `isolated=false`。

## 7. 卸载

```bash
rm -rf "$prefix/share/linux-sandbox" "$prefix/bin/sandbox" "$prefix/bin/sandbox-launcher"
```

状态目录默认在 `~/.local/state/linux-sandbox/`，由调用方自行清理。
