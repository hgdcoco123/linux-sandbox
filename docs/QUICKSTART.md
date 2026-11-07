# 5 分钟上手

只做一件事：**跑一段不可信代码，然后让程序告诉你它为什么没成功、下一步该看什么。**

## 0. 准备（一次）

```bash
git clone <this-repo> && cd <this-repo>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
bash tools/install.sh            # 装到 ~/.local（二进制 + SDK + sandbox-ai）
export PATH="$HOME/.local/bin:$PATH"
```

没有 systemd 用户会话、或不允许非特权 user namespace？**先跳到第 3 节的"只看结果"路径**，
那不需要隔离也能验证整条链路。

## 1. 看这台机器能跑哪种模式

```bash
sandbox doctor
```

看 `isolation_probe`：`available: true` 才能做隔离；否则它会告诉你卡在哪一步
（`userns` / `cgroup` / `make_mounts_private`）。这一步不是可选的，它决定后面用哪种方式跑。

## 2. 跑一段不可信代码并拿回结果

```bash
mkdir -p /tmp/demo-out
sandbox run \
  --runtime /path/to/runtime --cgroup-root "$SANDBOX_CGROUP_ROOT" \
  --input /tmp/demo-in --output /tmp/demo-out \
  --timeout 2000 --memory 256M \
  --summary -- /usr/bin/sandbox-fixture files
```

`--runtime` 是一个只含目标程序及其动态库的目录（怎么做见 [RUNTIME.md](RUNTIME.md)）；
`--cgroup-root` 指向一个被委派的空 cgroup 子树（在 `systemd-run -p Delegate=yes` 里运行时它就是
当前 cgroup）。

输出是六行人话：

```
verdict:    ok (success)
exit code:  0
output:     stdout 128 bytes, stderr 0 bytes
artifacts:  delivered (3 entries, 512 bytes), manifest artifact-manifest.json
time:       42 ms total (18 startup, 24 execution)
run:        run-3f2a…  (inspect with: sandbox inspect run-3f2a…)
```

`verdict=ok` 的含义是"目标正常退出**并且**产物已交付"——这是这个工具唯一想让你记住的语义。

## 3. 让程序解释这次运行

```bash
sandbox-ai --provider demo --db ~/.local/state/analysis.db --scope demo \
    explain --run-dir ~/.local/state/linux-sandbox/<run_id>
```

`--provider demo` 不需要任何凭据，输出是模板文本，用来确认链路通。换成真实模型：

```bash
export SANDBOX_AI_BASE_URL=... SANDBOX_AI_MODEL=... SANDBOX_AI_API_KEY=...
sandbox-ai --provider compatible --db ~/.local/state/analysis.db --scope demo \
    explain --run-dir ~/.local/state/linux-sandbox/<run_id>
```

它会打印：发生了什么（每条都指回某一行证据）、已确认、假设、未知、下一步。

### 只想看结果，不想配隔离

```bash
sandbox run --process --quiet --summary -- /bin/sh -c 'exit 3'
```

process 模式**没有隔离**（报告里 `isolated=false`），只用于开发与诊断；但它走同一套报告、
同一套 `checksums.json`、同一套分析流程，所以可以先把整条链路跑通。

## 4. 下一步

- 想接 CI：[INSTALL.md](INSTALL.md) 与 `action.yml`（如已提供）；
- 想接自己的程序：[RUNTIME.md](RUNTIME.md)；
- 想知道它防什么不防什么：[THREAT_MODEL.md](THREAT_MODEL.md)；
- 想知道哪些字段可以依赖：[CONTRACT.md](CONTRACT.md)；
- 想清理 /tmp 里堆起来的运行目录：`sandbox prune --keep 20 --dry-run`。

## 常见卡点

| 现象 | 原因 | 处理 |
|---|---|---|
| `isolation_probe.stage = make_mounts_private, error = Permission denied` | 不允许非特权 user namespace（Ubuntu 23.10+ 默认） | 临时放宽：`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`（用完恢复），或换一台允许的机器 |
| `stage = cgroup` | 没有委派的 cgroup 子树 | 在 `systemd-run --user -p Delegate=yes` 里运行，或显式传 `--cgroup-root` |
| `launcher is not executable` | 没装或目录分离 | `bash tools/install.sh`，保证 `sandbox` 与 `sandbox-launcher` 在同一目录 |
| 报告里 `workspace.export.ok = false` | 产物没交出来 | **这就是失败**，不是"没检查"；`errors` 里有原因 |
