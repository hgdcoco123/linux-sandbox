# 在 CI 里用

`action.yml` 把"跑一条不可信命令并判定结果"封成一个 composite action：它构建、准备 runtime、
在 `systemd-run --user -p Delegate=yes` 里跑一次隔离执行，然后按 `verdict` 决定这一步成功还是失败，
并把报告表格写进 job summary。

## 最小用法

```yaml
- uses: actions/checkout@v4
- uses: ./
  with:
    command: /usr/bin/sandbox-fixture proc-view
    explain: 'true'
- name: 断言产物已交付
  if: always()
  run: |
    python3 - <<'PY'
    import json
    report = json.load(open('sandbox-report.json'))
    assert report['workspace']['export']['ok'], 'artifacts were not delivered'
    PY
```

输出：`verdict`、`run-id`、`artifacts-ok`、`report`（报告路径）。

## 前置条件（很重要）

| 条件 | 不满足时 |
|---|---|
| runner 有 **user systemd**（GitHub 的 `ubuntu-*` runner 是带 systemd 的 VM，满足） | 无法委派 cgroup，action 会退回 `process` **仅当**你显式设 `allow-process: true` |
| 允许**非特权 user namespace** | Ubuntu 23.10+ 默认限制；`sandbox doctor` 会报 `make_mounts_private: Permission denied`，action 失败并打印该 stage |
| 提供 `runtime`（目标程序的根文件系统） | 不提供则用测试 fixture 建一个最小 runtime，只够冒烟 |

`allow-process: true` 会让这一步跑在**没有隔离**的诊断后端上（报告里 `isolated=false`）——适合先验证
CI 接线，不适合当安全边界。

## 报告在哪里

- `sandbox-report.json`：完整报告（稳定字段见 [CONTRACT.md](CONTRACT.md)）；
- `/output`（默认 `sandbox-output/`）：产物，附 `artifact-manifest.json`（每个文件的 sha256）；
- job summary：一张小表 + 一段分析层解释（`explain: false` 可关）。

## 失败语义

`sandbox-run.txt` 里是 `verdict / summary / exit_code / run_id / artifacts_ok`；`ci_run.py` 的退出码是
`0`（verdict=ok）、`3`（未成功）、`2`（配置或环境错误）。action 默认 `fail-on: not-ok`，把 `3` 变成
这一步失败——这正是 CI 需要的语义：**产物没交出来，这一步就不该绿**。

## 为什么不是 Docker action

隔离依赖 cgroup 委派与 user namespace；Docker action 里没有 systemd，也没有可用的委派子树，
只能 `--privileged` 才能工作。composite action 直接跑在 runner 上，用的就是 runner 自己的 systemd。

## 已验证 / 未验证

- **已验证（2026-11-07，真机）**：`tools/ci_run.py` 在 `systemd-run --user -p Delegate=yes` 里跑真实隔离
  运行，输出 `verdict=ok`、`summary=success`、`exit_code=0`、`artifacts_ok=true`、
  `manifest=artifact-manifest.json`；同一台机器上，缺委派或环境不允许 user namespace 时它会**拒绝**
  并点名 stage（`isolation unavailable at stage make_mounts_private: Permission denied`）。
  测试时临时放宽过 `kernel.apparmor_restrict_unprivileged_userns`（跑完恢复）——这是 Ubuntu 23.10+
  的前置条件，不是 action 的开关。
- **未验证**：在 GitHub 托管的 runner 上跑一遍（本机验证的是同一个机制，但没有走 GitHub 的
  checkout/inputs 外壳）。
