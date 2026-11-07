# 试一遍（10 分钟）

只做一件事：**跑一段程序，拿到判定与产物；再让分析层告诉你这次为什么没成功、下一步看什么。**

两条路，先看第 2 步的结果再选：

| 路线 | 需要什么 | 能验证到什么 |
|---|---|---|
| **A. 诊断后端** | 只要能编译 | 判定语义、报告、校验和、分析层（**没有隔离**） |
| **B. 隔离执行** | 一个已委派的 cgroup 子树 | 上面全部 + 真隔离、文件边界、产物原子交付 |

## 1. 取代码并构建

```bash
git clone https://github.com/hgdcoco123/linux-sandbox.git
cd linux-sandbox
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

依赖：C++20 编译器、CMake ≥ 3.21、`libseccomp-dev`、protobuf。

## 2. 看这台机器能跑哪种

```console
$ ./build/sandbox doctor
```

关心 `isolation_probe`：

```
"available": true,  "stage": "ready"                      → 走路线 B
"available": false, "stage": "make_mounts_private",
                    "error": "Permission denied"           → 只能走路线 A
```

`state_root` 是运行目录的位置（默认 `/tmp/sbx-<uid>`），后面会用到。

## 3A. 路线 A：两分钟看到全部机制

```console
$ ./build/sandbox run --process --quiet --summary -- /bin/sh -c 'echo hello; exit 3'
verdict:    target_failed (nonzero_exit)
exit code:  3
output:     stdout 6 bytes, stderr 0 bytes
time:       53 ms total (13 startup, 22 execution)
run:        run-a3ab9ba8df4775cd65f6459a  (inspect with: sandbox inspect run-a3ab9ba8df4775cd65f6459a)
exit:       10 (0 means success)
```

两行要分清：`verdict` 是"这次算不算成功"，`exit:` 是沙箱自己的进程退出码。

**取报告**（JSON，完整事实）：

```bash
./build/sandbox inspect run-a3ab9ba8df4775cd65f6459a
```

**让分析层解释**（不需要 API 凭据，用 `demo` 提供方确认链路）：

```console
$ PYTHONPATH=. python3 -m sandbox_ai --provider demo --db /tmp/a.db --scope demo \
      explain --run-dir /tmp/sbx-$(id -u)/run-a3ab9ba8df4775cd65f6459a
run: run-a3ab9ba8df4775cd65f6459a
execution: nonzero_exit, verdict target_failed, exit 3
status: completed

[已确认] 执行报告包含以下记录。
    ev-218bca01de94c918a36b5ee1 L2: "cleanup": {
...
```

`ev-…` 是证据编号，`L2` 是行号——**每条结论都能指回原文某一行**，这是分析层的核心设计。

## 4B. 路线 B：真隔离执行

前提：第 2 步 `available: true`。

```bash
# 1) 一个可用的 cgroup 子树
runs=$(python3 tools/cgroup_setup.py)

# 2) 目标程序的根文件系统（这里用随源码提供的测试程序）
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output /tmp/rt

# 3) 输入目录（files 模式会检查 /input 是只读的）
mkdir -p /tmp/in /tmp/out && echo "unchanged" > /tmp/in/sample.txt

# 4) 跑
./build/sandbox run \
  --runtime /tmp/rt \
  --cgroup-root "$runs" \
  --input /tmp/in \
  --output /tmp/out \
  --timeout 5000 --memory 256M --summary \
  -- /usr/bin/sandbox-fixture files
```

期望（关键是多出 `artifacts` 一行）：

```
verdict:    ok (success)
exit code:  0
output:     stdout 27 bytes, stderr 0 bytes
artifacts:  delivered (1 entries, 10 bytes), manifest artifact-manifest.json
time:       ... ms total (... startup, ... execution)
run:        run-...
exit:       0 (0 means success)
```

```console
$ cat /tmp/out/result.txt          # 产物确实落到了宿主目录
converted
```

**这条命令的形状和仓库验收测试、CI 里跑的完全一致**（`tests/acceptance/isolation.py`），
所以它通过意味着隔离、文件边界、产物交付三件事都成立。

`/input` 是只读的：目标往 `/input` 写会失败——这是特性，用来验证边界。

## 5. 换成真实模型（可选）

```bash
export SANDBOX_AI_BASE_URL=...  SANDBOX_AI_MODEL=...  SANDBOX_AI_API_KEY=...
PYTHONPATH=. python3 -m sandbox_ai --provider compatible --db /tmp/a.db --scope demo \
    explain --run-dir /tmp/sbx-$(id -u)/<run_id>
```

用 `bash tools/install.sh` 或 `pip install .` 之后可以直接使用 `sandbox-ai` 命令。

## 6. 卡住了看这里

| 现象 | 原因 | 处理 |
|---|---|---|
| `available: false, stage: make_mounts_private` | 内核不允许非特权 user namespace（Ubuntu 23.10+ 默认） | 先用路线 A；要真隔离需放宽该 sysctl 或换机器（见 [INSTALL.md](INSTALL.md) 3.1） |
| `available: false, stage: cgroup` | 没有委派的 cgroup 子树 | `python3 tools/cgroup_setup.py`，或在 `systemd-run --user -p Delegate=yes` 里运行 |
| 目标 `startup_failure`，stderr 为空 | runtime 里缺解释器/动态库 | 用 [RUNTIME.md](RUNTIME.md) 的方法准备 |
| `artifacts: NOT delivered` | 产物没交出来 | 这就是失败；看 `workspace.export` 与 `errors[]` |
| `launcher is not executable` | `sandbox` 与 `sandbox-launcher` 不在同一目录 | 用 `bash tools/install.sh` 安装，并使用安装后的路径 |

## 7. 接下来

- 完整用法（参数、会话、批处理、清理）：[USAGE.md](USAGE.md)
- 报告字段能不能依赖：[CONTRACT.md](CONTRACT.md)
- 它防什么不防什么：[THREAT_MODEL.md](THREAT_MODEL.md)
- 接 CI：[CI.md](CI.md)
