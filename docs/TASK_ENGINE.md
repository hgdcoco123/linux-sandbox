# 0.5 本机任务执行引擎

本轮沿用已有 Linux 隔离、单次执行和串行会话，增加四项核心实现：类型化命令模型、有界输出流水线、共享事件等待入口、批次内有界调度。CLI/Python SDK 均可使用。

## 模块关系

```text
CLI / Python SDK
  ├─ run → RunSession
  ├─ session-open → SessionController → CommandExecution
  └─ batch → Scheduler + ResourceBudget → 独立 sandbox run 子进程
                  │
       EventLoop：控制 socket / signalfd / timerfd / 通道 / 输出 / eventfd
                  │
       OutputCapture → BoundedWriter → FileOutputSink
                  │
       launcher → init → 目标和后代
```

批次控制器使用 posix_spawn 启动多个独立监督进程，不在多线程进程中 fork 后执行复杂 C++ 逻辑。
每个目标仍由已有 launcher/init 管理。单个连续会话仍只允许一条活动命令，batch 任务只支持一次执行，不共享持久 Shell。

## 类型模型与协议边界

`command_model.hpp/cpp`：

- RequestId 与 CommandId 是不同类型，受控 parse 工厂校验字符串。
- ExecuteRequest 解析 argv、可选 cwd/env、chrono::milliseconds 超时；保留字段是否省略，以维持原请求去重语义。
- resolve_command 的主要接口接收 ExecuteRequest，在可信 RunConfig 范围内生成有效配置。
- CommandFacts 在内部 COMMAND_DONE 边界解析 ID、退出码、信号、启动 errno、StopReason 和执行时间。
- CommandRecord 持有明确字段和 CommandPhase；apply 拒绝重复或错误命令事实；finish 要求输出 EOF 和写入完成。

JSON 保留在协议、配置、存储和可扩展指标边界，并未消除全部 JSON。现有 v2 外部字段和请求去重仍兼容，新增输出字段为增量字段。
`task.hpp/cpp` 中 TaskSpec、TaskState、ResourceClaim、ResourceBudget、Scheduler 组成批次内部模型。

## 输出流水线与所有权

每条 stdout/stderr 使用独立 BoundedWriter 和一个工作线程。FIFO 队列传递移动的 vector<char>；只有工作线程访问 OutputSink，只有监督线程更新命令/会话生命周期。

```text
管道可读 → 监督线程读（每轮最多 64KiB）→ 有界队列
                                           ↓
                                      写盘工作线程
                                           ↓
                                 更新 WriterStatus + eventfd
                                           ↓
                                监督线程检查完成/失败
```

队列容量每流 256KiB，包含正在写入的块；单次读取块最大 16KiB。另有读取缓冲、容器节点、线程栈等开销，不能把 512KiB 当成整个命令的内存上限。

队列满：监督线程暂停该流读取，并暂时移除其可读 FD 监听；目标可能阻塞在 pipe 写入。工作线程释放容量后通过 eventfd 唤醒监督器。控制、信号、期限仍可以被处理。
达到 capture_bytes：停止保存新数据，继续读取并计入 discarded，沿用原截断语义。队列满本身不静默丢日志。

完成规则：管道 EOF 仅表示生产结束；必须等队列排空、写线程 finished 且无错误，才能发布正常命令结果。
短写循环处理；写盘失败传播至监督器，执行不能被报告为成功。状态包含已写字节、失败字节及错误文本。
`flushed` 表示排队数据已通过 write 交给操作系统，不表示 fsync 或断电持久性。

关闭时停止接收新块，排空已接受块，再 join；不 detach。异常路径同样收回线程。线程创建发生在监督器安装信号处理之后，正常退出在恢复信号状态前 join。
**普通文件 write 可能陷入不可中断内核 I/O，join 不承诺硬期限。** 本轮提高监督循环对慢写入的独立性，不保证硬实时关闭。
状态快照/fsync、内部日志、文件上传导出仍可能是同步 I/O，不能声称监督线程已经完全没有阻塞操作。

报告增加 `flushed`、`queue_peak_bytes`、`backpressure_events`、`failed_bytes`、`write_error`。
连续会话保留旧 `seen/stored`；单次执行保留旧 `saved_bytes/discarded_bytes/eof`。

## 事件入口

RunSession、SessionController、batch 共用 EventLoop（epoll）。它没有长期捕获对象的回调，不存借用对象指针，仅返回当前监听集合的 tag。
每轮根据活动对象更新 FD 集合；MOD 遇到 ENOENT 时重新 ADD，覆盖 FD 已关闭后整数被复用的情况。
输出容量不足时不监听该管道；写入完成 eventfd、控制请求、内部事实、signalfd、timerfd 共同唤醒监督器。
每轮管道读取有预算，内部协议最多处理固定数量消息，避免单一源无限占用。20ms 定时器保留，用于期限和未完成控制请求检查。
本轮没有通用 Reactor 框架，也未把每个局部 poll（例如 init 内部）全部替换。

## 批次调度

manifest：

```json
{
  "schema_version": 1,
  "max_parallel": 2,
  "memory_budget_bytes": 134217728,
  "pids_budget": 64,
  "tasks": [
    {"id":"test-a", "config": {
      "schema_version":1, "mode":"process", "network":"host",
      "cwd":"/tmp", "argv":["/bin/true"],
      "limits":{"memory_bytes":67108864,"pids":32}
    }}
  ]
}
```

配置文件读取上限 1MiB，任务数 1..256，并发 1..32；每个 config 使用现有 RunConfig 格式。
任务 ID 1..32 个 ASCII 字母/数字/连字符/下划线，不重复。一次 batch 的任务模式必须一致；拒绝持久会话任务和永远无法放入预算的任务。
isolated 批次拒绝不同任务之间重叠的可写挂载树；只读输入可共享。宿主仍负责不并发修改输入/运行时，process 诊断任务不能得到文件隔离保证。

FIFO：队首放不下就等待，不用小任务绕过它。这样规则简单，但可能存在队首阻塞。
admit 在启动前预留并发名额、配置 memory_bytes 和 pids；supervisor 退出并被 waitpid 回收后才释放。启动失败回滚预留，单个任务失败不阻止其余任务。
这些数字是批次内准入预算，不是整机硬配额；不包含监督进程、写线程等宿主开销，不跨多个 batch 协调。process 模式仅测试调度，不提供 cgroup 硬限制。

状态：queued → starting → running → finishing → completed；失败到 failed。queued 可以直接 cancelled；活动取消通过 pidfd 向该任务监督进程发 SIGTERM，仍等待其已有清理流程结束才释放预算。
已经完成的任务即使晚到取消请求也保留真实结果，cancel_requested 不强行覆盖已完成事实。
批次只有最终 summary 已发布才显示 finished；常规中间快照不会提前宣布完成。

## 使用方式

本地受控诊断示例（明确不隔离）：

```bash
./build/sandbox batch --config profiles/batch-process-demo.json --allow-unsafe
```

正式隔离批次（manifest 中每个 config 提供 runtime_root；包装器统一填入委派 cgroup_root）：

```bash
/opt/linux-sandbox/bin/sandbox-run --batch --config /absolute/path/batch.json
```

低层 batch 已有委派时直接使用；`session-request` 的控制客户端也可连接批次，支持 status、cancel、cancel-task：

```json
{"protocol_version":2,"op":"cancel-task","task_id":"test-a"}
```

大型批次的整体状态可能超过 16KiB 控制响应限制。可读取 batch.json，或用 `{"op":"status","task_id":"test-a"}` 查询单项。

Python：

```python
from sdk.batch_client import BatchClient

client = BatchClient('/opt/linux-sandbox/bin/sandbox', '/tmp/my-agent-state')
batch = client.start('/absolute/path/batch.json')
task = batch.task('test-a')
print(task.status())
# task.cancel()      # 取消单项；batch.cancel() 取消整个批次
summary = batch.wait(timeout=60)
print(task.result())
print(task.output_path('stdout'))
```

wait 超时仅停止等待，不取消任务。丢弃句柄不会关闭任务。使用 task.output_path 可按偏移读取正在生成的文件；还没有专门的异步输出 SDK。
SDK auto 自动选择已有委派或 systemd 用户服务；已安装 SDK 的 batch 启动验收由 installed_sdk.py 覆盖。

## 文件布局、恢复和清理边界

批次元数据在用户指定 state_root/run-ID：owner.json、batch.json、result.json、tasks/任务ID/config.json。
任务执行记录保留在独立短路径 `/tmp/sbx-tasks-XXXXXX/序号/run-ID`，避免嵌套 run-ID 超出 UNIX socket 路径限制。
此目录由 mkdtemp 以 0700 创建，绝对路径记入批次 owner.json/batch.json 的 task_state_root，各任务也给出自身 task_state_root/run_id。
supervisor.stdout/stderr 是每个执行监督进程的输出，目标日志在其 run-ID/stdout.bin、stderr.bin。

批次不自动删除记录或产物，没有自动续跑/接管。批次监督器遭 SIGKILL 时，子监督器可能继续按各自期限运行；查询和回收应检查记录里的任务根目录，不能把缺少批次终态理解为任务未执行。
单条连续会话的请求去重/恢复仍然保留，不能把它扩大解释成批次整体恰好执行一次。
本轮未增加 root 常驻服务、全局调度器、数据库、远程 RPC、无锁队列或内存池。

## 测试与学习

先读 command_model/task 的模型测试，再读 BoundedWriter 和 writer_tests 的 Gate，最后连接 OutputCapture/EventLoop、batch 和已有两个监督循环。
性能实验和局限见 PERFORMANCE.md；当前 Linux 运行验收状态见 VALIDATION.md。
