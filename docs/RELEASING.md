# 发布一个版本

版本号来自 `CMakeLists.txt` 的 `project(... VERSION)`，`sandbox --version` 与 `pyproject.toml`
里的 `version` 都要与它一致。

## 1. 改版本

```bash
# CMakeLists.txt: project(linux_sandbox VERSION x.y.z ...)
# pyproject.toml: version = "x.y.z"
# CHANGELOG.md: 增加 x.y.z 一节（把"未发布"里的条目移进去）
```

## 2. 验证（发布前必须全绿）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
bash verify.sh                 # CTest + 分析层契约 + demo + 打包元数据
# 隔离路径需要一次真机演练（要 sudo 放宽 userns，脚本会自动恢复）：
bash tools/ai-verify-e2e.sh
```

## 3. 打包

```bash
bash tools/pack.sh
# dist/linux-sandbox-x.y.z-linux-x86_64.tar.gz
# dist/linux-sandbox-x.y.z-linux-x86_64.tar.gz.sha256
```

包内自带 `bin/`、`share/linux-sandbox/`（SDK、辅助脚本、分析层）、`docs/`、`LICENSE`、`CHANGELOG.md`。

## 4. 打标签并发布

发布由 `.github/workflows/release.yml` 完成：tag push 触发，runner 构建同一个 tarball、跑 `ctest`、
用仓库自带的 `GITHUB_TOKEN` 建 Release 并上传附件。**不需要任何人的本地 `gh` 登录。**

```bash
# 正文写在这里，workflow 会用它（找不到就退回 CHANGELOG.md）
$EDITOR docs/releases/vx.y.z.md

git tag -a vx.y.z -m "x.y.z"
git push origin vx.y.z      # 触发 release.yml
```

然后到 Actions 里看 `release` 这个 run，它结束时会打印 `gh release view` 的输出（正文 + 资产列表）。
手动重发一次（tag 已存在时）：

```bash
gh workflow run release.yml -f tag=vx.y.z
```

如果确实要在本地发（有 `gh` 且已登录），等价命令：

```bash
gh release create vx.y.z dist/*.tar.gz dist/*.sha256 \
  --title "x.y.z" --notes-file docs/releases/vx.y.z.md --verify-tag
```

Release notes 里请保留"已验证 / 未验证"两段：这个项目对外最有价值的不是功能列表，而是知道哪些结论
有测试和真机证据、哪些没有。

## 5. 发布后自检

```bash
d=$(mktemp -d) && tar -xzf dist/linux-sandbox-*.tar.gz -C "$d"
"$d"/linux-sandbox-*/bin/sandbox --version          # 与标签一致
"$d"/linux-sandbox-*/bin/sandbox-ai --help >/dev/null && echo "AI layer importable"
( cd "$d" && sha256sum -c ../dist/*.sha256 2>/dev/null || true )
```

## 回滚

Tarball 与 tag 都是不可变的；出问题时**发一个新版本**，不要移动旧 tag。报告格式的兼容规则见
[CONTRACT.md](CONTRACT.md)（新增字段不升版本，改语义必须升版本）。
