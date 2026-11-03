# 交接：单次执行与批量任务的 workspace 配额（决策 A）

> **2026-11 更新：已实现。** 清单里的 7 步都落在代码里了：`run_session` 建 transfer 目录并
> 以 fd 发 `TransferChannel`、`init::run_once` 在目标结束后导出 `final`、`rootfs` 跳过
> `/output` bind、报告新增 `workspace{...}`（报告 schema 1→2）、`tests/acceptance/isolation.py` 加了
> 配额与"导出被拒绝"两条并进了 `tools/isolated-accept.sh` 必检行。**还没做的只剩下"请用户在
> 虚拟机里跑一遍 `bash tools/isolated-accept.sh`"**（需要 sudo 改 AppArmor 开关，Agent 做不了）。
> 细节与三处对清单的修正见 [HANDOFF](HANDOFF.md) §4.7，下面保留原始清单作为决策记录。
>
> 三处与原文不同、且是有意的：
> 1. **没有动 `record_schema_version`**：命令记录的形状没变（`FinishedCommand` 没加字段），
>    加一档只会制造一个假版本。变的是**报告**的 `schema_version`（1→2，多了 `workspace`），
>    读侧仍按"字段缺失 = 未知"处理。原文把两套版本混成了一件事。
> 2. **"谁发布"原文没写**：导出落在 `<run>/transfer/final`，必须再由 `run_session` 用
>    `renameat2(RENAME_EXCHANGE)` 换到宿主目的地（跨文件系统时先拷到同盘再交换），
>    否则产物永远到不了调用方目录。
> 3. **只有"确实 exec 过"才导出**：目标没跑起来（启动/准备失败）时导出会把调用方目录
>    换成一棵空树，等于失败还毁掉了旧产物；因此 `attempted` 也只在 `ExecObserved` 之后才为真。

这份文档只讲**剩下的一件事**和做它需要知道的一切。整体状态、设计历史、踩坑清单在
`docs/HANDOFF.md`、`docs/REDESIGN.md`、`docs/REMAINING.md`，先读那三份再读这份。

---

## 0. 现状（不要重做已完成的部分）

| 项 | 值 |
|---|---|
| 权威仓库 | `D:\cpp_project\linux-sandbox`（**不要直接改这里**） |
| 工作副本 | `C:\Users\lipinzheng\documents\deepseek-harness\default-workspace\sbx` |
| 当前提交 | 决策 A 已实现，提交号看 `git log -1`；权威仓库与工作副本必须一致、工作区干净 |
| VM 工作树 | `~/sbx2/linux-sandbox`（测试用）、`~/projects/linux-sandbox`（另一份） |
| 进程模式测试 | `ctest --test-dir build` → **33/33**（`isolation_integration` 无委派时 skip） |
| 干净重编警告 | **0**（`cmake -S . -B build-warn && cmake --build build-warn`） |
| 隔离验收 | **本轮修复后全绿**：`bash tools/isolated-accept.sh` 必检行全齐、`NOT EXECUTED` 为 0、服务 `code=exited/status=0`；真机上踩到的两个坑见 `docs/HANDOFF.md` §4.7 |

已经**完成并验证**的东西，不要回退、不要重新设计：

- 会话 workspace 是 tmpfs，`size=workspace.max_bytes`、`nr_inodes=workspace.max_entries`，
  **会话累计**、内核强制、无扫描代码
- 宿主传输目录以 **fd** 交给沙箱（`TransferChannel` 消息），沙箱内**不存在 `/transfer` 路径**
- 发布是 `renameat2(RENAME_EXCHANGE)` 原子交换，旧副本在交换前完好
- 传输有序号、单在途、超时作废通道、迟到回执按序号丢弃
- 复制可被 `Stop`/`Cancel` 中断（`TransferAborted`），**中止 ≠ 超时**：中止不作废通道
- 结果里执行/导出/清理三类事实分开记录

---

## 1. 唯一剩余任务：决策 A

**已定语义**：隔离模式下 `/output` **一律是 tmpfs**（会话、单次执行、批量任务同一条磁盘边界）。
因此 `config.mounts[target=/output].source` 的含义从"工作区本身"变成**最后一次导出落地的目录**。
批量任务的子进程就是 `sandbox run`（`src/runtime/batch.cpp:302` 一带），所以批量自动继承。

**这是对外契约变更**，改动必须同时更新文档，见第 4 节。

---

## 2. 实现清单（顺序即依赖，每步都要能编译）

1. **`src/isolation/rootfs.cpp`** —— 配置了 `workspace_transfer` 时**跳过** `/output` 的 bind。
   现在 transfer 分支先挂 tmpfs，随后 `for (const auto &m : c.mounts)` 会把调用方目录 bind
   **盖在 tmpfs 之上**，配额静默消失。这是最容易漏的一步。
2. **`src/runtime/run_session.cpp`** —— 当 `config_.mounts` 里有 `target == "/output"` 时：
   - 创建 `<run>/transfer` 与 `<run>/transfer/in`（0700）
   - 把 `/output` 挂载项的 `source` 记成导出目的地，并从 `config_.mounts` 里移除该项
   - `config_.workspace_transfer = <run>/transfer`
   - 在发送 `Prepare{}` 之后（约 L350）打开该目录并发 `TransferChannel{}` + fd
3. **`src/runtime/init.cpp` 的 `run_once`** —— 在 `prepare_environment` 之后、`EnvReady` 之前
   接收 `TransferChannel`（照 `run_persistent` 的写法，见 L378 附近）；`supervise_command`
   返回后先 `export_workspace(fd, "final", config.workspace)`，再 `control.send(Transferred{...})`，
   最后 `InitDone`。中止回调传空（实现时确实如此；**理由不是"没有通道"**——通道在，只是单次执行
   选择不在导出期间轮询它，超时由监督端的预算兜底。会话路径则在等待期间处理信号，见下方注记）。
4. **`include/sandbox/model/report.hpp` + `src/model/report.cpp` + `src/core/record_json.cpp`** ——
   `RunReport` 增加 `Json workspace`（仅在有导出时非空），内容
   `{"export":{"attempted","ok","bytes","entries","error"}}`；`record_schema_version` 加一档，
   读侧保持向后兼容（老记录没有这个字段）。
5. **`run_session` 收 `Transferred`** —— 记进 `report_.workspace`；`ok == false` 时进 `errors`
   并让 `verdict` 反映"产物未交付"，**不得**因为目标退出码 0 就报成功；init 在导出前死亡同样
   走这条。
6. **测试** —— `tests/acceptance/isolation.py`：现有关于 `/output` 的断言改为"运行结束后产物出现在同一
   宿主目录"（产物仍在那里，只是**结束时**才出现）；新增一条单次执行的配额耗尽用例
   （fixture 的 `fill-bytes` 模式，超预算必须拿到 `60`）。
7. **文档** —— `LIMITATIONS` 里关于 `/output` 的两句话、`HANDOFF` §1 状态表、
   `ACCEPTANCE` §3 期望行与 §6 覆盖清单。

**验收标准（缺一不可）**：

- 进程模式 `ctest` 全绿（新增用例后是 34/34 或更多）
- `bash tools/isolated-accept.sh` 全绿，且新的单次执行配额用例在必检行里
- 报告在导出失败时不报成功（用 `keep_on_close` 或封锁 transfer 目录制造失败）

---

## 3. 这次踩过的坑（别重犯）

| 坑 | 现象 | 正确做法 |
|---|---|---|
| 能力在 fork 之前就丢了 | 给目标 `unshare(CLONE_NEWNS)` 遮蔽 `/transfer` → `EPERM`，所有隔离命令启动失败 | `prepare_environment()` 里先 `drop_capabilities()`；想让目标看不见某路径，**就别让它出现在文件系统里**（fd + `*at`），不要遮挡 |
| 回调污染 `errno` | 复制结束时误报 `read artifact directory: Resource temporarily unavailable`（`EAGAIN`） | 回调前后保存/恢复 `errno`；`tests/integration/publish_tests.cpp` 里有对应回归用例 |
| `systemd-run` 不继承环境 | `export SBX_TEST_ARTIFACTS=...` 在单元里不存在，日志指向从未创建的目录 | 用参数显式传（`delegated_run.py --artifacts`），或 `-E` |
| CMake 选项加在目标定义之前 | 静默无效，警告一条没少 | 选项必须写在**目标已定义之后** |
| 1ms 超时不是测试 | 复制太快，随机不超时 | 用 `SBX_TRANSFER_WAIT_MS=0`（"完全不给时间"） |
| 大段替换吞掉函数 | `init.cpp` 的 `run_persistent` 开头被吃掉、`rootfs.cpp` 花括号配平坏掉 | 先 `git checkout -- <file>` 恢复，再**按行号**精确替换，替换后立刻读回确认 |
| CRLF | 12 个文件磁盘是 CRLF、索引是 LF，`git status` 却报干净 | 提交前 `-replace "\r\n","\n"`；脚本类文件必须 LF |
| 拿不到 sudo | 隔离验收需要改 `kernel.apparmor_restrict_unprivileged_userns` | 只能**请用户**跑 `bash tools/isolated-accept.sh`；跳过不算通过 |

---

## 4. 工作流（这套环境怎么干活）

**改代码 → 同步到 VM → 编译测试 → 提交 → 合并 → 再同步**

```powershell
# 1. 推送改动的文件到 VM 的 ~/sbxctl/f，再展开到两棵树
$ip='192.168.244.128'
foreach($rel in @('src/runtime/run_session.cpp')) {
  Invoke-WebRequest -Uri "http://$ip`:8000/put/f/$rel" -Method POST `
    -Body ([IO.File]::ReadAllBytes((Join-Path $clone ($rel -replace '/','\')))) `
    -ContentType 'application/octet-stream' -UseBasicParsing | Out-Null
}

# 2. 在 VM 上展开、编译、跑测试
Invoke-WebRequest -Uri "http://$ip`:8000/exec" -Method POST -Body @'
for tree in ~/sbx2/linux-sandbox ~/projects/linux-sandbox; do
  ( cd ~/sbxctl/f && find . -type f -exec cp --parents {} "$tree/" \; )
done
cd ~/sbx2/linux-sandbox
cmake --build build --parallel 2 2>&1 | grep -E "error:" | head
ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"
'@ -ContentType 'text/plain' -UseBasicParsing

# 3. 绿了才提交，然后快进到 D:
git add -A; git commit -F <message file>
git -C 'D:\cpp_project\linux-sandbox' fetch $clone master
git -C 'D:\cpp_project\linux-sandbox' merge --ff-only FETCH_HEAD
```

**隔离验证只能请用户跑**（需要 sudo）：

```bash
cd ~/sbx2/linux-sandbox && bash tools/isolated-accept.sh
```

它会保存/恢复 sysctl、按原值恢复、**跳过即判失败**，失败时把会话现场留在
`/tmp/sbx-acc.XXXX/artifacts`（含 `result.json`、`internal.log`、`host.stderr`）。

---

## 5. 不要做的事

- 不要重新讨论 A/B：**A 已定**（见 `REMAINING.md` 的对应条目）
- 不要在没跑隔离验收的情况下说"隔离路径通过"
- 不要为每个步骤造新类：传输/工作区各有**一个**组件就够
- 不要为了凑数字加测试：`session_safety` 那组假测试刚被删掉，测试数从 34 降到 33 是**好事**
- 不要把 `/output` 的双方（调用方目录、tmpfs）都当成"工作区"——只有一个工作区，另一个是导出目的地
