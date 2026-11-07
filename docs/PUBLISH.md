# 发布到 GitHub

目标：别人能 `git clone`，也能在 CI 里一行 `uses:`。下面的顺序不能颠倒——**先让两个 workflow
在干净 runner 上绿，再打 tag**。

## 0. 发布前自检（本地）

```bash
git status --short          # 干净
bash verify.sh              # CTest + 分析层契约 + demo + 打包元数据
bash tools/pack.sh          # 确认打包能过（产物在 dist/，已被 .gitignore 忽略）
```

## 1. 推仓库

```bash
git remote add origin git@github.com:<你>/linux-sandbox.git
git push -u origin master
```

推上去会自动跑两个 workflow：

| workflow | 它验证什么 |
|---|---|
| `linux.yml` | 构建 + `ctest` + 在 `systemd-run --user -p Delegate=yes` 里跑命名空间/cgroup 行为检查 |
| `action-smoke.yml` | `sandbox doctor` 的探针 + **action 本身**（`uses: ./`）+ 报告结构断言 |

**两个都绿再往下走。** 如果 `action-smoke` 红在 `isolation: False stage: make_mounts_private`，
那是 runner 禁止非特权 user namespace——不是 action 的 bug，记到 README 的已知限制里再继续。

## 2. 打 tag 前检查

| 检查 | 命令 / 位置 |
|---|---|
| `action.yml` 在仓库根目录 | `ls action.yml` |
| `name` / `description` / `branding` 合法 | `action.yml` 头部 |
| 根目录有 `LICENSE` | `ls LICENSE` |
| 版本一致 | `grep VERSION CMakeLists.txt`、`grep version pyproject.toml`、`sandbox --version` |
| `CHANGELOG.md` 有新版本一节 | `grep "^## " CHANGELOG.md` |
| 没有提交构建产物或报告 | `.gitignore` 已覆盖 `build*/`、`dist/`、`sandbox-report.json`、`sandbox-output/` |

## 3. 打 tag 并建 Release

```bash
git tag -a v0.5.0 -m "0.5.0"
git push origin v0.5.0

bash tools/pack.sh
gh release create v0.5.0 dist/*.tar.gz dist/*.sha256 \
  --title "0.5.0" \
  --notes-file <(sed -n '/^## 0.5.0/,/^## /p' CHANGELOG.md)
```

Release 里**同时**给 tarball（不想编译的人）和源码（`uses:` 的人）。notes 里保留"已验证 / 未验证"两段。

## 4. 发布后验证（模拟别人）

```bash
# 1) 别人 clone 能不能建
d=$(mktemp -d) && git clone --depth 1 --branch v0.5.0 <repo> "$d/linux-sandbox"
cd "$d/linux-sandbox" && cmake -S . -B build >/dev/null && cmake --build build --parallel 2 >/dev/null \
  && ./build/sandbox --version

# 2) 别人的 CI 能不能用（在另一个仓库里）
cat > .github/workflows/try-sandbox.yml <<'YAML'
on: [workflow_dispatch]
jobs:
  run:
    runs-on: ubuntu-24.04
    steps:
      - uses: <你>/linux-sandbox@v0.5.0
        with:
          command: /usr/bin/sandbox-fixture proc-view
YAML

# 3) tarball 能不能装
tar -xzf dist/linux-sandbox-0.5.0-linux-x86_64.tar.gz -C /tmp
/tmp/linux-sandbox-0.5.0-linux-x86_64/bin/sandbox --version
```

## 5. 别人怎么用（写进 README 的两种形式）

```yaml
# 固定版本（推荐）
- uses: <你>/linux-sandbox@v0.5.0

# 跟随最新（会被你的改动影响，不建议）
- uses: <你>/linux-sandbox@master
```

## 6. 发布之后的承诺

| 承诺 | 出处 |
|---|---|
| 报告字段分级（稳定 / 实验 / 诊断） | [CONTRACT.md](CONTRACT.md) |
| 新增可选字段不升版本，改语义必须升版本 | 同上 |
| tag 不移动，回滚靠发新版本 | [RELEASING.md](RELEASING.md) |
| 安全边界（防什么、不防什么） | [THREAT_MODEL.md](THREAT_MODEL.md) |
| 已知未验证项 | [VERIFY_ELSEWHERE.md](VERIFY_ELSEWHERE.md)、`AI_VALIDATION.md` |

## 6.5 发到 PyPI（让陌生用户能 `pip install`）

GitHub Releases 在国内经常连不上；PyPI 有大量镜像（清华、阿里…），是"陌生用户真的能拿到"的通道。
从 0.5.3 起仓库支持把它打成**平台 wheel**，里面带静态二进制：

```bash
bash tools/build_wheel.sh                 # 需要先有静态构建：-DSANDBOX_STATIC_DEPS=ON
# -> dist/linux_sandbox-<ver>-py3-none-linux_x86_64.whl
```

脚本会检查：二进制与 `pyproject.toml` 版本一致、二进制确实静态链接、wheel 里有 `sandbox` 与
`sandbox-launcher` 且可执行、wheel 不是 `py3-none-any`（防止被装到 macOS 上）。

上传（需要 pypi.org 账号与 token）：

```bash
python3 -m pip install --user twine
python3 -m twine upload dist/linux_sandbox-<ver>-py3-none-linux_x86_64.whl
```

发布后用户侧：

```bash
pip install linux-sandbox        # 得到 sandbox / sbx / sandbox-ai 三个命令
sandbox --version
```

注意两点：

- wheel 的 tag 是 `linux_x86_64`，pip 只在 Linux x86_64 上装；其他架构会被正确地拒绝而不是装完才发现不能跑；
- 二进制仍要求 glibc ≥ 2.38（构建于 Ubuntu 24.04），与 tarball 一致。

## 7. 常见失败

| 现象 | 原因 | 处理 |
|---|---|---|
| `linux.yml` 红在 `find_package(Protobuf)` | runner 没装开发包 | workflow 里已 `apt-get install libseccomp-dev protobuf-compiler libprotobuf-dev`；确认没被缓存跳过 |
| `action-smoke` 红在 `isolation unavailable` | runner 限制 user namespace | 记录限制，保持 `--allow-process` 的诚实降级路径，不要为了变绿去伪造隔离 |
| `uses: <你>/linux-sandbox@v0.5.0` 报 `repository not found` | 仓库私有或名字写错 | 公开仓库，或在同组织内使用 |
| tarball 在别人机器上报 `GLIBC_2.xx not found` | 动态链接 glibc 版本不匹配 | 记录最低 glibc 要求；需要时提供静态构建 |
