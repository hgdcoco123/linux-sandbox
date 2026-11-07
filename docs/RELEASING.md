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

## 4. 打标签并上传

```bash
git tag -a vx.y.z -m "x.y.z"
git push origin vx.y.z

gh release create vx.y.z dist/*.tar.gz dist/*.sha256 \
  --title "x.y.z" --notes-file <(sed -n '/^## x.y.z/,/^## /p' CHANGELOG.md)
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
