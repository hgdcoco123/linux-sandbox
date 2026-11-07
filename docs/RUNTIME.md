# 给沙箱准备一个 runtime

`runtime_root` 是沙箱在隔离命名空间里能看到的**根文件系统**：目标程序、它需要的动态库、以及
`/bin/sh` 之类的解释器。沙箱不复制宿主机的 `/usr`，所以你必须显式准备它。

## 1. 最省事的形态：一个静态二进制

```bash
mkdir -p /srv/linux-sandbox/runtime/bin
gcc -static -O2 -o /tmp/hello hello.c          # 任何静态程序
install -m 0755 /tmp/hello /srv/linux-sandbox/runtime/bin/hello
```

配置里写 `"runtime_root": "/srv/linux-sandbox/runtime"`，目标写 `["/bin/hello"]`。
这足以跑通"执行 + 限额 + 产物导出"，也是仓库里 `tools/make_runtime.py --fixture` 做的事
（它只是把测试用的 `sandbox-fixture` 放进去）。

## 2. 想跑普通动态链接程序：把依赖一起拷进去

```bash
target=/usr/bin/python3
root=/srv/linux-sandbox/runtime
mkdir -p "$root$(dirname "$target")" "$root/lib" "$root/lib64"
install -m 0755 "$target" "$root$target"
# 拷贝所有依赖（不要跟随符号链接，保留 soname）
ldd "$target" | awk '/=> \// {print $3} /^\s*\// {print $1}' | sort -u | while read -r lib; do
    mkdir -p "$root$(dirname "$lib")"
    cp -L "$lib" "$root$lib"
done
```

注意三点：

1. **解释器路径**要存在（`/lib64/ld-linux-x86-64.so.2` 等），否则 exec 会在 stage 3 报
   `ENOENT`——报告里的 `exec_error` 会写成 `stage=3, errno=2`；
2. 需要 `/dev/null`、`/dev/urandom` 的话，在 runtime 里建同名节点或让沙箱提供（见
   `src/isolation/rootfs.cpp`）；
3. 不要把宿主的 `/etc`、`/home` 拷进去——那是把攻击面一起搬进沙箱。

## 3. 输入与输出

| 目标 | 怎么挂 | 说明 |
|---|---|---|
| `/input` | `"mounts": [{"source": "...", "target": "/input", "readonly": true}]` | 只读；`workspace.input = "snapshot_copy"` 时会在运行前拷贝一份进运行目录，改不动原目录 |
| `/output` | `"mounts": [{"source": "...", "target": "/output", "readonly": false}]` | 目标写这里；运行结束由沙箱**原子发布**到该目录，并生成 `artifact-manifest.json`（含每个文件的 sha256） |
| `/work` | tmpfs 配额 | 默认工作目录；`workspace.max_bytes` / `max_entries` 限制它，配额打满时目标会收到 `ENOSPC` |

产物发布语义：**`workspace.export.ok = false` 时整次运行不算成功**，报告会写明是导出被拒、超时，
还是沙箱在导出前就结束了。

## 4. 验证 runtime 是否够用

```bash
sandbox run --config /dev/stdin <<'JSON'
{"schema_version":1,"mode":"isolated","network":"none",
 "runtime_root":"/srv/linux-sandbox/runtime","cgroup_root":"/sys/fs/cgroup/…/runs",
 "argv":["/bin/hello"]}
JSON
```

看报告：`exec_error` 非空 → runtime 缺东西（`errno=2` 是找不到程序或解释器，`errno=13` 是权限）；
`verdict=ok` 说明执行链完整。`sandbox validate --config ...` 可以在不执行的情况下先做静态检查。

## 5. 常见错误

| 报告 | 含义 | 处理 |
|---|---|---|
| `exec_error: stage=3, errno=2` | exec 找不到程序或动态解释器 | 补 `runtime_root` 里的路径；用 `ldd` 对照 |
| `summary=startup_timeout` | init 在启动窗口内没观察到 exec | 调大 `limits.startup_ms`，或检查 runtime 是否被阻塞（如网络挂载） |
| `verdict=harness_error` + `workspace.export.error` | 产物没交付 | 看 `errors`；`/output` 源目录不可写或跨文件系统发布失败 |
| `signal=SIGSYS` | 目标用了 seccomp 基线策略不允许的 syscall | 换实现方式，或按 `docs/THREAT_MODEL.md` 评估策略 |
