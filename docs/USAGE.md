# 使用手册

面向"我要用它跑东西"的读者。安装见 [INSTALL.md](INSTALL.md)，5 分钟版本见 [QUICKSTART.md](QUICKSTART.md)，
报告字段的完整定义见 [CONTRACT.md](CONTRACT.md)。

## 1. 一次运行由四件事组成

```
runtime_root          目标程序及其依赖（只读挂载）
      │
cgroup_root ──► 执行 ──► 判定 verdict ──► 产物 + 报告
      │            │
      │            └── stdout / stderr / internal.log
      └── 资源上限的来源（cpu / memory / pids / tmpfs）
```

记住三条：

1. **`runtime_root` 是目标程序的"根文件系统"**，不是工作目录。没有它就跑不了隔离模式。
2. **`cgroup_root` 决定资源上限从哪来**，它必须是已委派的 cgroup v2 子树。
3. **`verdict` 才是"这次算不算成功"**，`exit_code` 只是目标进程的退出码。

## 2. 选择启动模式

```console
$ sandbox doctor
```

看 `isolation_probe`：

| 字段 | 含义 |
|---|---|
| `available: true` | 可以跑隔离模式 |
| `available: false` + `stage` | 卡在哪一步（`make_mounts_private`、`cgroup`、`userns`…） |
| `execution_verified` | 是否真实执行过一次启动探针 |
| `state_root` | 运行目录所在位置（默认 `/tmp/sbx-<uid>`） |

没有委派环境时只有 `--process` 可用。它是**诊断后端，没有隔离**，报告里 `isolated: false`。

## 3. 跑一条命令

```bash
sandbox run \
  --runtime /srv/runtime \
  --cgroup-root "$(python3 tools/cgroup_setup.py)" \
  --input ./input \
  --output ./out \
  --timeout 30000 \
  --memory 512M \
  --summary \
  -- /usr/bin/my-program --flag
```

### 常用参数

| 参数 | 作用 | 默认 |
|---|---|---|
| `--runtime DIR` | 目标程序的根文件系统 | 无，隔离模式必需 |
| `--cgroup-root DIR` | 已委派的 cgroup 子树 | 无，隔离模式必需 |
| `--input DIR` | 只读挂载到 `/input` | 无 |
| `--output DIR` | 可写挂载到 `/output`，运行结束原子发布 | 无 |
| `--timeout MS` | 墙钟上限，超时 SIGTERM → SIGKILL | 60000 |
| `--memory K\|M\|G` | 内存上限 | 不限 |
| `--process` | 诊断后端（隐含 `--allow-unsafe`） | 关 |
| `--quiet` | 不打印实时日志（`internal.log` 仍记录） | 关 |
| `--summary` | 六行人类可读结论 | 关 |

`--config FILE` 可以表达快速形式给不了的东西（mounts、env、seccomp、workspace 配额、会话）。
字段含义见 [CONTRACT.md](CONTRACT.md)。

### 读写文件

```bash
sandbox run --runtime ./rt --cgroup-root ./runs \
  --input ./data --output ./result \
  --summary -- /usr/bin/convert --source /input --target /output
```

- `/input` 只读：目标写入会失败（这是设计，用于验证边界）；
- `/output` 在运行结束时才发布，发布是原子的：失败时 `./result` 保持原样；
- 交付结果在报告的 `workspace.export`：`ok`、`entries`、`bytes`、`manifest`、`durable`。

## 4. 读懂结果

```console
$ sandbox run --process --summary -- /bin/sh -c 'echo hello; exit 3'
verdict:    target_failed (nonzero_exit)
exit code:  3
output:     stdout 6 bytes, stderr 0 bytes
time:       52 ms total (12 startup, 22 execution)
run:        run-9363156593973836fb6d6dd1  (inspect with: sandbox inspect run-9363156593973836fb6d6dd1)
exit:       10 (0 means success)
```

两行最容易被误读，先说清楚：

- **`verdict`**：`ok` / `target_failed` / `harness_error` / `cleanup_incomplete`
- **`exit:`**：沙箱自己的进程退出码（0/10/11/12/124/125/130/2），给 shell 和 CI 用

| verdict | 含义 | 你该做什么 |
|---|---|---|
| `ok` | 目标正常结束**且**产物已交付 | 读产物 |
| `target_failed` | 目标自己失败了（退出码非零 / 信号 / 启动失败） | 看目标的问题 |
| `harness_error` | 沙箱自身出错（配置、准备步骤、控制通道） | 看 `errors[]` 与 `internal.log` |
| `cleanup_incomplete` | 判定有效，但清理没干净 | 先 `sandbox cleanup`，再查残留 |

**成功不等于退出码 0**：退出 0 但产物没交付，仍然是 `target_failed`。

## 5. 用报告

```bash
sandbox inspect run-93631...            # 打印状态与报告
sandbox inspect run-93631... > report.json
```

```bash
python3 - <<'PY'
import json, subprocess
report = json.loads(subprocess.check_output(["sandbox", "inspect", "run-93631..."]))
print(report["verdict"], report["workspace"]["export"]["ok"], report["timing"]["total_ms"])
PY
```

稳定字段/实验字段/诊断字段的分级写在 [CONTRACT.md](CONTRACT.md)。**只依赖稳定字段**，升级才不会打断你。

运行目录里还可以直接读文件：

```
<state-root>/<run_id>/          # 默认 <state-root> = /tmp/sbx-<uid>
├── result.json          完整报告
├── config.json          本次使用的配置（或 request.json）
├── stdout.bin           原始字节
├── stderr.bin
├── internal.log         生命周期日志（--quiet 也记录）
├── checksums.json       上述文件的 sha256
└── artifact-manifest.json   产物清单（交付时）
```

## 6. 常见任务

### 限制 CPU 与并发

```json
{
  "schema_version": 1,
  "argv": ["/usr/bin/python3", "/work/script.py"],
  "runtime_root": "/srv/runtime",
  "cgroup_root": "/sys/fs/cgroup/user.slice/.../sandbox-runs",
  "limits": {"wall_ms": 60000, "startup_ms": 10000, "memory_bytes": 536870912, "pids": 32,
             "cpu_quota_us": 20000, "cpu_period_us": 100000},
  "mounts": [{"source": "/srv/work", "target": "/work", "readonly": true}]
}
```

### 连续执行多条命令（会话）

同一个隔离环境里顺序执行，文件状态保留：

```bash
sandbox session-open --config session.json          # 返回 run_id
sandbox session-request <run_id> --request step-1.json
sandbox session-request <run_id> --request step-2.json
```

Python：

```python
from sdk.session_client import SessionClient
with SessionClient(scope="demo") as session:
    print(session.execute(["/bin/sh", "-c", "echo hi > value"]).wait(5))
    print(session.execute(["/bin/cat", "value"]).output()["stdout"])
```

### 让分析层解释一次失败

```bash
sandbox-ai --provider compatible \
    --db ~/.local/state/analysis.db --scope demo \
    explain --run-dir /tmp/sbx-1000/run-93631...
```

输出结构：`summary` / `facts` / `hypotheses` / `missing_info` / `next_steps`，每条都带 `{id, line}` 引用。
没有 API 凭据时可以先用 `--provider demo` 看数据格式。

### 清理磁盘

```bash
sandbox prune --keep 20 --older-than 7d --dry-run    # 先看清单
sandbox prune --keep 20 --older-than 7d              # 只删已结束的运行
```

`prune` 不碰运行中的运行；`cleanup` 只做资源回收，不删文件。

## 7. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `isolated mode requires runtime_root and cgroup_root` | 缺参数 | 补 `--runtime` / `--cgroup-root`，或明确用 `--process` |
| `doctor` 报 `make_mounts_private: Permission denied` | AppArmor 限制非特权 user namespace（Ubuntu 23.10+ 默认） | `doctor` 会在 stderr 直接给出命令；详见 [INSTALL.md](INSTALL.md) 3.0 |
| 报 `cgroup ... no controllers` | 子树没委派 cpu/memory/pids | 用 `systemd-run --user -p Delegate=yes` 包一层，或 `tools/cgroup_setup.py` |
| 目标 `startup_failure`，stderr 空 | runtime 里缺解释器或动态库 | `ldd` 检查；用 [RUNTIME.md](RUNTIME.md) 的方法准备 runtime |
| 目标启动失败但没有输出 | 目标在 exec 前就被拒绝（seccomp/权限） | 看 `internal.log` 与 `errors[]` |
| `verdict: harness_error` | 沙箱自身问题 | 看 `errors[]`；提 issue 时带上完整运行目录 |
| 产物没交付但目标退出 0 | `/output` 写入失败或配额超限 | 看 `workspace.export.ok` 与 `errors[]` |
| 超时后仍占用资源 | 清理未完成 | `sandbox cleanup`，再看 `cleanup{}` 三个事实 |

## 8. 集成

**CI**：`uses: hgdcoco123/linux-sandbox@v0.5.0`，`verdict != ok` 时这一步失败。见 [CI.md](CI.md)。

**Python 里调用**：

```python
import json, subprocess
```

```python
report = json.loads(subprocess.check_output([
    "sandbox", "run", "--runtime", "/srv/runtime", "--cgroup-root", "/sys/fs/cgroup/.../runs",
    "--output", "./out", "--", "/usr/bin/my-program"]))
if report["verdict"] != "ok":
    raise SystemExit(f"{report['verdict']}: {report['summary']}")
```

**读报告而不是解析日志**：日志是给人看的，字段是给你程序用的。

## 9. 退出码

| 码 | 含义 |
|---|---|
| 0 | 成功（`verdict: ok`） |
| 10 | 目标非零退出 |
| 11 | 目标被信号终止 |
| 12 | 目标启动失败 |
| 124 | 超时 |
| 125 | 清理未完成 |
| 130 | 被取消 |
| 2 | 用法/配置错误（沙箱自身） |

## 10. 下一步

- 报告字段： [CONTRACT.md](CONTRACT.md)
- 安全边界： [THREAT_MODEL.md](THREAT_MODEL.md)
- 分析层细节： [AI_ANALYSIS.md](AI_ANALYSIS.md)、[AI_VALIDATION.md](AI_VALIDATION.md)
