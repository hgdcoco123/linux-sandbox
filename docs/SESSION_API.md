# 会话接口手册

## 0.5 兼容说明

execute v2 请求和请求去重语义保留，内部转为 ExecuteRequest/CommandRecord 类型。
结果的 stdout/stderr 额外提供 flushed、queue_peak_bytes、backpressure_events、failed_bytes、write_error。
flushed 指所有排队数据已 write 完成，不是 fsync；EOF 不再单独作为正常结果发布条件。
单个会话仍串行；独立批次入口见 `sandbox batch --help`，Python 侧为 `sdk.batch_client.BatchClient`。


## 0.4 接口更新

本版使用外部控制协议 v2；下面未带版本的 JSON 示例仍作为兼容 v1 请求可用。
新客户端 execute 使用 `protocol_version: 2` 和稳定的 `request_id`；同 ID 不同参数报 REQUEST_CONFLICT。
通过 `{"protocol_version":2,"op":"request-status","request_id":"build-1"}` 查询接受记录和命令结果，关闭后仍可通过 CLI/SDK 查询。
控制协议的错误与恢复约定见 [CONTRACT.md](CONTRACT.md) 与 `include/sandbox/core/control.hpp`。

SDK：`client.create(config, startup="auto")` 自动复用显式 cgroup_root，否则使用 systemd 用户委派；
`startup="direct"` 表示由调用者准备环境，`startup="delegated"` 强制创建委派服务。
`session.execute(argv, request_id="task-build-1")` 返回带 command_id/request_id 的句柄。
省略 ID 时自动生成；首次失败可读取异常的 request_id，SDK 不自动重试有副作用的请求。
`session.request_status("task-build-1")` 可返回活动命令、已保存终态或 uncertain。
`SandboxError.code` 和 `retryable` 提供机器可读分类。

状态新增 draining/failed；stdin 和带 ID 的 cancel 只作用于匹配的活动命令。
不自动迁移或接管旧活动会话；升级前关闭会话并同步更新 CLI、SDK。


## 1. CLI

```text
sandbox session-open --config FILE [--state-root DIR] [--launcher PATH] [--allow-unsafe]
sandbox session-request RUN_ID --request FILE [--state-root DIR]
sandbox files --request FILE
```

session-open 为前台监督进程，关闭后才输出最终 JSON。通过 state-root/run-ID/session.json 发现会话和就绪状态。
返回 0 表示会话正常收尾，125 表示监督或清理错误，2 表示入口错误。
单条命令失败不会直接使整个会话失败，读取该命令结果判断。
session-request 从 JSON 文件读取请求，stdout 输出响应 data；请求错误返回 2。
files 操作可信本地目录，不启动沙箱。

## 2. 配置

```json
"session": {"enabled": true, "total_ms": 3600000, "idle_ms": 300000}
```

0 < idle_ms <= total_ms <= 86400000。session-open 强制开启，run 拒绝 enabled=true。
配置 argv 仍用于结构校验，必须提供绝对程序路径，但不会自动执行。
limits.wall_ms 是每条命令的最大期限；会话资源上限持续作用于 init 和全部命令。
isolated 会话不允许配置外部 /output 映射，该路径由软件管理。

## 3. 请求字段

以下每行是独立请求文件的完整内容：

```json
{"op":"status"}
```

返回 session_state、current、last、commands 和会话指标。current / last 是摘要；完整命令结果通过 result 获取。

```json
{"op":"execute","argv":["/usr/bin/sandbox-fixture","echo"],"cwd":"/output","wall_ms":1000}
```

可选 env 对象；提供时替换本条命令环境。返回 command_id，例如 cmd-1。
同一会话严格串行，正在执行或收尾时拒绝新命令。argv[0] 必须是绝对路径，不隐式执行 Shell。

```json
{"op":"result","command_id":"cmd-1"}
```

未结算返回 state=pending。结算后包含 state、argv、cwd、exit_code、signal、exec_errno、stop_reason、execution_ms、stdout、stderr、metrics。
正常主进程退出 stop_reason=main_exited；成功需 exit_code=0、signal=null、exec_errno=null。
其他 stop_reason 包括 timeout、cancelled、startup_failure。
state=interrupted 表示无法完整观察执行结果，退出信息可能为 null。

```json
{"op":"stdin","command_id":"cmd-1","bytes":[104,105,10]}
```

返回 accepted_bytes，可能为部分字节或 0，调用方必须保留未接受内容并重试。
SDK 使用 Protobuf bytes，单次最多 4096 字节；最终编码后的整个数据包上限为 16 KiB。CLI 仍接收 JSON，JSON 请求文件本身也受 16 KiB 限制。

```json
{"op":"stdin-close","command_id":"cmd-1"}
```

关闭输入才表示 EOF；命令结束也会自动关闭输入。

```json
{"op":"output","command_id":"cmd-1","stream":"stdout","offset":0}
```

返回 bytes 数组、offset、next_offset、finished，每次最多 2048 字节。
stdout/stderr 分别维护偏移，无全局严格顺序。
finished 表示命令已结算，不表示该次已经读尽，应继续读取直至空 bytes。
超过 capture_bytes 的输出已丢弃，无法通过偏移读取。

```json
{"op":"cancel","command_id":"cmd-1"}
```

提供 ID 时必须匹配当前命令，防止旧句柄取消后续命令。不提供 ID 则取消当前命令，兼容原 cancel CLI。

```json
{"op":"close"}
```

关闭整个会话。accepted 仅表示接收，应观察至 closed。
关闭后控制 socket 移除，最终 JSON 和命令文件仍保留。

## 4. 会话文件交付

仅 isolated 且 idle 时允许；上传目录名必须是单个新目录组件。

```json
{"op":"files","request":{"op":"snapshot","source":"/home/user/source","name":"src"}}
```

对应 /output/src，不覆盖已有目录。

```json
{"op":"files","request":{"op":"manifest"}}
```

```json
{"op":"files","request":{"op":"export","destination":"/home/user/new-results"}}
```

导出整个工作区到一个尚不存在的目录。
响应含 bytes、entries、entry_count、manifest_truncated；复制成功还含 complete 和 destination。
控制响应最多返回前 24 项，较长路径会进一步减少数量；完整结果在 artifacts.json。
普通文件项含 path/type/size/sha256，目录项含 path/type。清单没有排序保证。

## 5. 离线目录命令

```json
{"op":"snapshot","source":"/home/user/input","destination":"/home/user/new-snapshot"}
```

```json
{"op":"manifest","source":"/home/user/input"}
```

```json
{"op":"export","source":"/home/user/results","destination":"/home/user/new-export"}
```

将请求存为文件后运行 sandbox files --request FILE。
路径来源必须是可信本地用户，不能直接向不可信远程调用方开放。
限制、禁止链接、权限与复制失败语义见 [THREAT_MODEL.md](THREAT_MODEL.md)。

## 6. 本地传输协议

- UNIX SOCK_SEQPACKET，单请求单响应，SO_PEERCRED 校验同 UID。
- 状态目录私有，socket 权限 0600；最大请求/响应 16KiB。
- 每轮最多接收 8 个连接，最多保留 16 个待处理连接，等待请求上限 500ms。
- 客户端收发超时 5 秒。响应为 {"ok":true,"data":...} 或 {"ok":false,"error":...}。
- CLI 解开封装，错误写 stderr。原始字符串 cancel 请求保留兼容。
- 没有恰好一次语义；响应丢失后不要盲目重试 execute，先查询 current/last 和记录。
- 同步文件传输可能在客户端超时后继续进行，重试前检查状态和目标。
- 协议当前未承诺长期稳定版本；不是远程多租户 API。

## 7. Python SDK

```python
from sdk.session_client import SessionClient
client = SessionClient("./build/sandbox", "/tmp/sbx-client")
with client.create("job.json") as session:
    session.upload("./source", "src")
    command = session.execute(["/usr/bin/python3", "main.py"], cwd="/output/src", wall_ms=5000)
    print(command.wait(timeout=10))
    session.export("./new-results")
```

示例需自备含 Python 和依赖的 runtime，静态 fixture 不含 Python。
SessionHandle 提供 status/execute/upload/manifest/export/close/request。
CommandHandle 提供 result/wait/output/write/close_stdin/cancel。
wait 超时抛 TimeoutError，命令继续；通信错误为 SandboxError；目标非零退出体现在结果字段。
with 退出时 close，丢弃句柄不自动销毁环境。
client.attach(原 state_root, run_id) 可重连；SDK 为每次创建生成私有子状态目录。普通会话控制请求直接通过 UNIX SOCK_SEQPACKET 使用 Protobuf，CLI 的请求文件和输出仍是 JSON。旧 JSON socket 请求仍受支持；批次控制继续使用原 JSON 协议。关闭后的 request-status 可经 CLI 核验并从持久化记录查询。
SessionHandle.state_root 和 run_id 应由调用方保存以便重连。
关闭后 SDK 可从落盘结果和输出读取历史。
没有推送订阅、任意文件编辑 RPC、传输进度、单文件选择导出或 C++ SDK。
