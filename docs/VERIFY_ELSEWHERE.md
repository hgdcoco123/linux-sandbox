# 需要在别的机器上跑的两项验证

这两项在本项目的开发环境里做不到，但都不需要改代码。任何有条件的机器跑一次，把输出贴回
issue 或提交里即可。

## 1. GitHub 托管 runner 上的 action

**为什么本机做不了**：本机验证的是 `tools/ci_run.py` 这一步（同一个委派机制），没有走 GitHub
的 `checkout` / inputs 外壳。

**怎么跑**：把仓库推到 GitHub，启用 `.github/workflows/action-smoke.yml`（推送到任意分支即触发）。

**期望**：

```
isolation: True
verdict=ok
artifacts-ok=true
```

如果 runner 禁止非特权 user namespace，`doctor` 会报 `stage: make_mounts_private`，action 会明确
失败并打印该 stage——这也是一个可接受的结果，请把那一行贴回来，它说明 runner 的环境限制。

## 2. `pip install .` 的 wheel 构建

**为什么本机做不了**：开发环境的解释器没有 pip，也没有 `python3-venv`。

**怎么跑**（任何有 pip 的 Linux/Windows/macOS）：

```bash
python3 -m venv /tmp/v && . /tmp/v/bin/activate
pip install .                      # 或 pip install ".[sdk]" 连带装 protobuf
sandbox-ai --help
sandbox-ai --provider demo --db /tmp/a.db --scope demo demo --output /tmp/demo-out
```

**期望**：`sandbox-ai --help` 有输出；demo 打印 `{"statuses": [...], "output": ...}` 并在
`/tmp/demo-out` 写出三份分析。

## 3.（可选）容器里的隔离路径

**怎么跑**：

```bash
docker run --rm -it --privileged -v "$PWD":/src -w /src ubuntu:24.04 bash
apt-get update && apt-get install -y cmake g++ libseccomp-dev protobuf-compiler libprotobuf-dev \
    libspdlog-dev nlohmann-json3-dev python3
cmake -S . -B build && cmake --build build --parallel
python3 tools/cgroup_setup.py          # 打印可用作 --cgroup-root 的路径
./build/sandbox doctor
```

**期望**：`cgroup_setup.py` 输出一个路径（而不是"controllers unavailable"），`doctor` 报
`available: true`。这条最不确定——容器运行时对 cgroup 命名空间的处理差异很大，所以请把两条输出都贴回。
