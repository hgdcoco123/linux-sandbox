# 实机验收手册

> **当前状态（0.6 重构，按版本记录，不要和下面历史段落混读）**：
> - Ubuntu 24.04 x86_64：`ctest` **33/33**；`tools/isolated-accept.sh` **全绿**——必检行全齐、
>   `NOT EXECUTED` 为 0、服务 `code=exited/status=0`、sysctl 按原值恢复。
> - 已验证到的行为：namespace/cgroup/seccomp 边界、`/input` 只读与 `snapshot_copy`、隔离模式
>   `/output` 的 tmpfs 配额（会话累计；单次执行/批量在结束时一次性导出）、`/transfer` 的 fd 化
>   双向移交、导出失败与超时/中断的报告语义。
> - **交付清单**（2026-11-07 演练）：隔离运行的报告里出现 `workspace.export.manifest = "artifact-manifest.json"`、`manifest_entries = 1`，清单里的 `sha256` 与落地文件 `verify-output/value` 的 `sha256sum` **一致**。
> - **没有**验证过：其他发行版/内核、崩溃与掉电恢复、长时间压力、真正的恶意代码。
>
> 下面第 2–3 节是复现路径；第 4–7 节是失败时回传什么、以及最容易出问题的地方。

## 1. 环境

Ubuntu 24.04 x86_64，cgroup v2，允许 user namespace。

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libseccomp-dev \
    protobuf-compiler libprotobuf-dev python3-protobuf
```

## 0. 一条命令收齐材料

```bash
bash tools/ubuntu-accept.sh          # 构建 + 测试 + 委派隔离 + 环境信息，打包成 tar.gz
bash tools/ubuntu-accept.sh --skip-iso   # 只想先看构建与常规测试
```

脚本不会修改主机（不装包、不改 sysctl、不动 AppArmor），失败也不中断，
只把每一步的退出码和输出收进一个目录。它最后会打印要回传的文件清单。

## 2. 构建与常规测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

期望：**构建零错误**；CTest 中除 `isolation_integration`（无委派时 skip）外全部通过。

不要因为 skip 就认为隔离没问题——它表示"没测"。

**覆盖口径**：`publish_tests` 的跨文件系统用例只在 `/dev/shm` 与 `/tmp` 是不同设备时才执行，
不满足时它会打印一行并正常返回。所以它的最终成功行**不证明 EXDEV 分支跑过**——验收记录必须抄下
这两行里的哪一行：

```text
cross-filesystem publish: exercised (EXDEV path, /dev/shm differs from /tmp)
cross-filesystem publish: NOT exercised (same device as /tmp); the EXDEV path is unverified on this machine
```

同理，`tests/integration/publish_tests.cpp` 里"持久性未确认"的用例是用"父目录不可读"模拟的，验证的是
"交换成功但无法确认持久性"这条分支，**不是** `fsync()` 返回 I/O 错误本身。

## 3. 委派下的隔离测试

```bash
test_root=$(mktemp -d /tmp/sbx-acc.XXXXXX)
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output "$test_root/runtime"
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --test-runtime "$test_root/runtime"
```

期望：`isolated integration passed`、
`single-run quota enforced and the partial tree was exported`、
`a refused export is reported, not passed off as success`、
`process sessions and artifact regression passed`、
`isolated session smoke passed`、`isolated /input snapshot passed`、
`workspace quota, export and failure reporting passed`、
`an external stop interrupts an in-flight export`、
`a timed-out transfer is reported and the channel is quarantined`、
`isolated batch smoke passed`、`Finished with result: success`。

更省事的方式：

```bash
bash tools/isolated-accept.sh
```

它保存当前的 `kernel.apparmor_restrict_unprivileged_userns`、临时放开、跑上面全套、按**原值**
恢复（成功、失败、Ctrl-C 都恢复），并且**把跳过当失败**：逐条核对上面的成功行，出现
`NOT EXECUTED` 直接判失败。


## 4. 出问题时请回传这些

1. 上面三条命令的**完整输出**（不要截断）
2. `uname -a`、`systemctl --version | head -1`、`cat /proc/self/cgroup`
3. 失败用例的 run 目录（默认 `/tmp/sbx-$(id -u)/run-*/`）里的
   `state.json`、`result.json`、`internal.log`
4. 隔离相关失败再加：`sudo dmesg | tail -40`、`journalctl --user -n 50 --no-pager`
5. `sudo aa-status | head -20`（如果怀疑是 AppArmor）

## 5. 最可能出问题的地方（按怀疑度排序）

重构在这几处动过真实逻辑，**静态检查通过不代表能跑**：

| 位置 | 症状 | 先看 |
|---|---|---|
| `src/runtime/run_session.cpp` 状态推进 | 单次执行在收尾时抛 `illegal transition`，或报告 `summary` 与退出码不符 | `state.json` 的 `state` 序列 |
| `core/supervisor` 完成条件 | 跑到目标结束后**永不返回**（等不到 EOF 或后代清空） | `internal.log` 最后几行；`cgroup.procs` 是否还有进程 |
| `src/runtime/protocol.cpp` FD 契约 | `FD_CONTRACT` / `BAD_PACKET` 错误，init 或 launcher 提前退出 | `internal.log` 中的 `control` 相关行 |
| `src/runtime/init_common.cpp` 准备顺序 | 挂载 EPERM/EINVAL，`ENV_READY` 从未到达 | `internal.log`；`prepare` 阶段的最后一条 |
| `src/storage/artifacts.cpp` 策略 | 上传/导出报 `entry_limit` / `byte_limit`，即使文件很小 | 请求里的 `workspace` 段是否被配置覆盖 |
| `core/prepare` 回滚 | 准备失败后残留 cgroup 或挂载点 | `/sys/fs/cgroup` 下是否有 `sbx-*` 残留 |
| 结果裁定链 | `run` 打印 `summary=success` 但退出码是 **125**；或批次任务由 completed 变 failed，原因写着 `task environment not reclaimed: cleanup_incomplete` | 这是新加的判定链：`summary` 只说目标结果，`verdict` 说环境是否可信，退出码跟 verdict。先看 `cleanup.errors` 与 `stdout.eof/flushed` |

## 5.5 新增的判定链（容易误读）

```text
summary  目标进程做了什么        success / timeout / cancelled / nonzero_exit …
verdict  这棵树值不值得信        ok / target_failed / harness_error / cleanup_incomplete
exit     调用方该不该继续        0 / 10 / 11 / 124 / 130 / 13 / 12 / 125
```

三者的关系是**刻意分开**的：目标退出 0 而清理没走完时，`summary=success`、
`verdict=cleanup_incomplete`、退出码 `125`。批次里同样的任务会被判成 failed，
错误信息是 `task environment not reclaimed: cleanup_incomplete`。

看到这种组合不要当成"报告自相矛盾"，它正是设计意图；要查的是**为什么清理没走完**
（通常是 `core/supervisor` 的完成条件没被满足：后代未清空或某条输出没有 EOF+落盘）。

## 6. 已知未覆盖

- 崩溃/掉电恢复、磁盘故障、长期泄漏、并发竞争
- `/input` 的 `snapshot_copy`、隔离会话 `/output` 的 tmpfs 配额（会话累计、内核强制）、
  `/transfer` 双向移交与导出失败报告：**均已端到端验过**（`0674c26`，`bash tools/isolated-accept.sh`
  全绿，服务 status=0）。不放开 AppArmor 开关时只能验宿主侧那半段（复制、原子发布、
  按 `keep_on_close` 保留/删除）
- 单次执行/批量任务的 `/output` tmpfs 配额与"运行结束时导出"（决策 A）：用例在
  `tests/acceptance/isolation.py`，必检行是 `single-run quota enforced and the partial tree was exported`
  与 `a refused export is reported, not passed off as success`。**已端到端验过**（本轮修复后
  `bash tools/isolated-accept.sh` 必检行全齐、`NOT EXECUTED` 为 0、服务 status=0）；
  OOM group kill 会连 init 一起杀掉，此时产物确实交付不了，报告会写"未交付"——这是对的，
  用例里 OOM 那条是 `export_ok=None`。"目标成功但产物未交付"时的 verdict/退出码含义见 §5.5。
- `session_wire` 的**回包方向**仍是「类型化记录 → 渲染成 JSON → Protobuf」，见 MIGRATION_SESSION 第 4 步
- 两份 pid-1 实现只剩环境准备共享，命令循环仍各写一份

## 7. 报告模板

```
内核/发行版：
构建：OK / 错误（贴末尾 30 行）
ctest：通过 N / 失败 M / 跳过 K
publish_tests 跨文件系统分支：exercised / NOT exercised（贴 §2 那一行）
隔离测试：通过 / 失败（贴输出）
首个失败用例：
  run 目录：
  result.json 摘要：
  internal.log 末尾：
补充：dmesg / journalctl / aa-status（如相关）
```
