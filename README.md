# linux-sandbox

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/hgdcoco123/linux-sandbox)](https://github.com/hgdcoco123/linux-sandbox/releases)
[![Build](https://github.com/hgdcoco123/linux-sandbox/actions/workflows/linux.yml/badge.svg)](https://github.com/hgdcoco123/linux-sandbox/actions/workflows/linux.yml)

Linux 上的不可信程序执行底座：**调用者拿到的不只是退出码，而是一份可核验的判定与产物清单**；
在它之上还有一个分析层，把一次失败的运行翻译成"发生了什么、缺什么、下一步"，并且每条结论都能指回某一行证据。

> `--cgroup-root` 必须指向一个已委派的 cgroup v2 子树；获取方式见 [docs/INSTALL.md](docs/INSTALL.md)，`sandbox doctor` 会告诉你这台机器支持哪种启动模式。

```console
$ sandbox run --runtime ./runtime --cgroup-root ./runs --output ./out --summary -- /bin/hello
verdict:    ok (success)
exit code:  0
output:     stdout 128 bytes, stderr 0 bytes
artifacts:  delivered (3 entries, 512 bytes), manifest artifact-manifest.json
time:       42 ms total (18 startup, 24 execution)
run:        run-3f2a...  (inspect with: sandbox inspect run-3f2a...)
exit:       0 (0 means success)
```

## 安装（一条命令，Linux x86_64）

```bash
base="https://gitee.com/lpzdinghai/linux-sandbox/releases/download/v0.5.3"; name="linux-sandbox-0.5.3-linux-x86_64"
cd /tmp && curl -fsSL -O "$base/$name.tar.gz" && curl -fsSL -O "$base/$name.tar.gz.sha256" && sha256sum -c "$name.tar.gz.sha256" && mkdir -p ~/.local && tar -xzf "$name.tar.gz" -C ~/.local && ~/.local/$name/install.sh
```

装到 `~/.local`，不需要 root / git / 编译器；上一条命令会校验 sha256、链接 `sandbox`/`sbx`/`sandbox-ai`、
跑一次冒烟测试，并告诉你这台机器能不能做隔离。装完新开终端（或 `source ~/.bashrc`）即可：

```console
$ sbx --version
sandbox 0.5.3
$ sbx run --process --quiet --summary -- /bin/sh -c 'echo hi; exit 3'
verdict:    target_failed (nonzero_exit)
exit:       10 (0 means success)      # 10 是预期判定，不是错误
```

GitHub 同理，把 `gitee.com/lpzdinghai/linux-sandbox` 换成 `github.com/hgdcoco123/linux-sandbox` 即可。
`pip install linux-sandbox` 尚未发布到 PyPI（见 [docs/PUBLISH.md](docs/PUBLISH.md) 6.5）。

## 与"直接 exec / 套个容器"的区别

沙箱的难点不在于把进程跑起来，而在于**这次运行到底算不算成功**。本项目把这个问题拆成几个可验证的事实：

| 保证 | 含义 |
|---|---|
| **成功 = 目标正常退出 且 产物已交付** | 产物导出失败、被截断、跨文件系统发布失败，都不会被当成成功 |
| **产物原子发布** | staging + `renameat2(RENAME_EXCHANGE)`，崩溃时不会出现半新半旧的目录 |
| **判定与事实分离** | `verdict`（这次运行算不算成功）与 `summary`（目标自己做了什么）分开，harness 故障不会被误报成目标失败 |
| **运行目录自证** | 每次运行写 `checksums.json`（报告、配置、stdout/stderr、生命周期日志的 sha256） |
| **资源硬上限** | cgroup v2 限制 cpu / memory / pids / tmpfs 字节与 inode，超时后 SIGTERM → SIGKILL |
| **清理可验证** | `cleanup{descendants_empty, cgroup_removed, launcher_reaped}` 是报告字段，不是承诺 |

## 分析层（`sandbox_ai`）

对一次运行给出结构化结论：`summary` / `facts` / `hypotheses` / `missing_info` / `next_steps`。
设计上的关键选择是**模型只指认位置，不书写引用**：

- 证据以带行号的文本提供（`L12| ...`），模型只能回 `{id, line}`，原文由程序解析；
- 越界、过短、未被提供过的行会被直接拒绝 —— 模型没有伪造引用文本的通道；
- 检索为 BM25（可选向量 + RRF）+ 可选 rerank；被问题点名的文件强制进入上下文；
- 可选 `--self-check`：再问一次模型"这条引用是否真的支持该结论"，只记录，不改变校验结果。

纯 Python、零第三方依赖，可单独安装使用（见下）。

## 快速开始

### 国内用户：从 Gitee 下载（不需要访问 GitHub）

```bash
base="https://gitee.com/lpzdinghai/linux-sandbox/releases/download/v0.5.3"
name="linux-sandbox-0.5.3-linux-x86_64"

curl -fL -o "$name.tar.gz"        "$base/$name.tar.gz"
curl -fL -o "$name.tar.gz.sha256" "$base/$name.tar.gz.sha256"
sha256sum -c "$name.tar.gz.sha256"          # 必须"成功"

mkdir -p ~/.local && tar -xzf "$name.tar.gz" -C ~/.local
~/.local/$name/install.sh
export PATH="$HOME/.local/bin:$PATH"
sandbox --version                            # sandbox 0.5.3
```

只要用 Python 那一层（`sandbox-ai`），也可以装 wheel——**先下载再安装**（Gitee 拒绝 pip 直接抓
release URL，会返回 403）：

```bash
curl -fL -O "$base/linux_sandbox-0.5.3-py3-none-manylinux_2_38_x86_64.whl"
pip install ./linux_sandbox-0.5.3-py3-none-manylinux_2_38_x86_64.whl
```

仓库地址：<https://gitee.com/lpzdinghai/linux-sandbox>（国内可达；CI 集成仍需 GitHub，`uses:` 只认 GitHub）。

### 预编译包

```bash
# 一条命令：下载 → 校验 sha256 → 装到 ~/.local → 冒烟测试 → 报告隔离是否可用
git clone --depth 1 https://github.com/hgdcoco123/linux-sandbox.git
sh linux-sandbox/tools/quick-install.sh
```

不想跑脚本就手动来（等价命令，需要自己 `sudo mkdir`）：

```bash
ver=0.5.3
file="linux-sandbox-${ver}-linux-x86_64.tar.gz"
curl -fL --retry 3 --retry-delay 2 -o "$file" \
  "https://github.com/hgdcoco123/linux-sandbox/releases/download/v${ver}/${file}"
test -s "$file" || { echo "download failed; see Troubleshooting below"; exit 1; }
sha256sum -c <(curl -fsSL "https://github.com/hgdcoco123/linux-sandbox/releases/download/v${ver}/${file}.sha256")
sudo mkdir -p /opt && sudo tar -xzf "$file" -C /opt
export PATH="/opt/linux-sandbox-${ver}-linux-x86_64/bin:$PATH"
sandbox doctor
```

### 从源码

```bash
git clone https://github.com/hgdcoco123/linux-sandbox.git && cd linux-sandbox
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
bash tools/install.sh       # 安装到 ~/.local，包含 sandbox 与 sandbox-ai
```

依赖：C++20 编译器、CMake ≥ 3.21、`libseccomp-dev`、`protobuf`（json 与 spdlog 已随源码提供）。

### 只要分析层

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install .               # 或 pip install ".[sdk]" 以启用会话 SDK
sandbox-ai --help
```

## 用法

**执行一条命令**（`--process` 为无权诊断后端，无隔离）：

```bash
sandbox run --process --quiet --summary -- /bin/sh -c 'echo hello; exit 3'
```

**解释一次失败**：

```bash
sandbox-ai --provider compatible --db ~/.local/state/analysis.db --scope demo \
    explain --run-dir ~/.local/state/linux-sandbox/<run_id>
```

**清理已完成的运行目录**：

```bash
sandbox prune --keep 20 --older-than 7d --dry-run   # 先看清单
sandbox prune --keep 20 --older-than 7d
```

**在 CI 里**：

```yaml
- uses: hgdcoco123/linux-sandbox@v0.5.0
  with:
    command: /usr/bin/sandbox-fixture proc-view
    timeout-ms: '3000'
```

步骤在 `verdict != ok` 时失败，报告与产物清单写入 job summary。详见 [docs/CI.md](docs/CI.md)。

## 工作方式

```
host
 ├── sandbox (CLI)              解析配置、写报告、管理状态目录
 │    └── sandbox-launcher      fork init，转入独立的 PID/mount/net namespace
 │         └── init (PID 1)     seccomp、资源归属、执行目标、回收子进程
 │              └── target      在 tmpfs 工作区运行，产物经 /output 导出
 └── run directory/            result.json、stdout/stderr、internal.log、checksums.json、产物清单
```

- 判定来自 `ExecutionCore`（类型化事件进、动作出），报告只是它的适配层；
- 产物导出在 init 停止前完成，通过 staging + 原子交换发布到宿主目录；
- 分析层消费运行目录，产出带行地址引用的结构化结论。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/USAGE.md](docs/USAGE.md) | 使用手册：参数、读结果、常见任务、故障排查、退出码 |
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 10 分钟试一遍：两条路线（无权限 / 真隔离）+ 逐条期望输出 |
| [docs/INSTALL.md](docs/INSTALL.md) | 安装、三种启动模式、无 systemd 环境、退出码 |
| [docs/RUNTIME.md](docs/RUNTIME.md) | 如何为目标程序准备 runtime（根文件系统） |
| [docs/CONTRACT.md](docs/CONTRACT.md) | 报告字段分级（稳定 / 实验 / 诊断）与语义承诺 |
| [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) | 防什么、不防什么，以及每条声明对应的测试 |
| [docs/VALUE.md](docs/VALUE.md) | 分析层是否真的帮到了"下一次尝试"（对照实验与数字） |
| [docs/AI_VALIDATION.md](docs/AI_VALIDATION.md) | 已执行 / 未执行的验证记录 |
| [docs/CI.md](docs/CI.md) | 在 CI 中使用，以及托管 runner 上的委派限制 |
| [docs/RELEASING.md](docs/RELEASING.md)、[docs/PUBLISH.md](docs/PUBLISH.md) | 发布流程 |

## 下载失败怎么办

发布包在 GitHub Releases 上。如果这台机器连不上 `github.com:443`（`curl: (35) SSL_ERROR_SYSCALL`、
`Connection reset`、超时都是这个原因），按顺序试：

1. **换一种下载方式**（不是网络问题的话，`-o` 明确写文件名比 `-O` 稳）：

   ```bash
   curl -fL --retry 3 --retry-delay 2 -o linux-sandbox-0.5.3-linux-x86_64.tar.gz \
     https://github.com/hgdcoco123/linux-sandbox/releases/download/v0.5.3/linux-sandbox-0.5.3-linux-x86_64.tar.gz
   ```

   注意是 `-O`（大写字母 O，保存到文件）而不是 `-0`（数字零，会让 curl 输出到终端，什么也不下载）。

2. **在能上网的机器上下载，再传过去**：

   ```bash
   # 能上网的机器
   sha256sum linux-sandbox-0.5.3-linux-x86_64.tar.gz
   scp linux-sandbox-0.5.3-linux-x86_64.tar.gz user@target:/tmp/
   # 目标机器，校对官方校验和
   cd /tmp && sha256sum -c linux-sandbox-0.5.3-linux-x86_64.tar.gz.sha256
   ```

3. **从源码构建**（同样需要一个能访问仓库的方式）：

   ```bash
   git clone https://github.com/hgdcoco123/linux-sandbox.git
   cd linux-sandbox && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSANDBOX_STATIC_DEPS=ON
   cmake --build build --parallel && bash tools/install.sh
   ```

4. 走代理/镜像时，只把 `github.com` 换成可达的镜像地址，**文件路径保持不变**；下载后务必用
   Release 页上的 `.sha256` 校验一次。

**没有 git、也连不上 GitHub？** 发布包自带安装器（0.5.2 起），只要有 `tar`：

```bash
sha256sum -c linux-sandbox-0.5.3-linux-x86_64.tar.gz.sha256    # 先校验
mkdir -p ~/.local && tar -xzf linux-sandbox-0.5.3-linux-x86_64.tar.gz -C ~/.local
~/.local/linux-sandbox-0.5.3-linux-x86_64/install.sh           # 链接 + 冒烟测试 + 报告隔离
export PATH="$HOME/.local/bin:$PATH"
sandbox --version
```

（详见 [docs/INSTALL.md](docs/INSTALL.md) 2.2；不需要 git、curl、root 或编译器。）

## 平台与要求


| 项 | 要求 |
|---|---|
| 系统 | Linux，cgroup v2；内核支持 user namespace |
| 权限 | 不需要 root；需要 user systemd 委派，或一个可写的 cgroup 子树 |
| 无以上条件 | 只能使用 `--process` 诊断后端（**不是安全边界**，报告里 `isolated=false`） |
| 已验证平台 | Ubuntu 24.04 x86_64（含 `apparmor_restrict_unprivileged_userns=1` 下用 AppArmor profile 只放行本 launcher 的隔离执行） |

## 状态

**已验证**：隔离执行与产物交付（含清单与校验和）、`ctest` 34/34、分析层 108 项契约测试、
GitHub Actions 上的构建与行为检查、pip 安装、预编译包、真机验证闭环演练。

**未验证**：其他发行版与内核、崩溃与掉电恢复、长时间压力、真实恶意代码；
分析层的语义充分性只有自评指标（未与人工标注对齐）；引用通过率是样本内结果。

**不在范围内**：多租户与鉴权、seccomp 策略 DSL、常驻守护进程、分布式调度、向量近邻索引。
`checksums.json` 用于发现事后误改，不防伪造（无签名）。

## 贡献

欢迎 issue 与 PR。修改报告字段前请阅读 [docs/CONTRACT.md](docs/CONTRACT.md)：稳定字段的语义变更需要提升
`schema_version`。安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。

## 许可证

[MIT](LICENSE)