# 实机验收手册

> 本仓库当前状态：**没有任何 Linux 运行时行为被验证过**。
> 本机能证明的只有：类型、接口、符号完整、可移植逻辑的行为（见 `tools/cross-verify.ps1`）。
> 这份文档是把"能证明的"变成"真的能用"的最短路径。

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

## 3. 委派下的隔离测试

```bash
test_root=$(mktemp -d /tmp/sbx-acc.XXXXXX)
python3 tools/make_runtime.py --fixture build/sandbox-fixture --output "$test_root/runtime"
systemd-run --user --wait --pipe --collect -p Delegate=yes -p Type=exec \
  /usr/bin/python3 "$PWD/tools/delegated_run.py" \
  --sandbox "$PWD/build/sandbox" --test-runtime "$test_root/runtime"
```

期望：`isolated integration passed`、`process sessions and artifact regression passed`、
`isolated session smoke passed`、`Finished with result: success`。

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
- `/input` 的 `snapshot_copy`：**复制这一步**已在宿主侧验证（`session-open` 失败前 run 目录里出现 `<run>/input`，内容与宿主一致、宿主后续修改不可见、按 `keep_on_close` 保留或删除）；**挂进沙箱 namespace 那一段**只在允许非特权 userns 的机器上可验，本机 AppArmor 阻塞，`sessions.py` 的 isolated 分支已有断言
- `session_wire` 仍是 Protobuf → JSON → 业务层 → JSON → Protobuf
- 两份 pid-1 实现只剩环境准备共享，命令循环仍各写一份

## 7. 报告模板

```
内核/发行版：
构建：OK / 错误（贴末尾 30 行）
ctest：通过 N / 失败 M / 跳过 K
隔离测试：通过 / 失败（贴输出）
首个失败用例：
  run 目录：
  result.json 摘要：
  internal.log 末尾：
补充：dmesg / journalctl / aa-status（如相关）
```
