# 输出流水线的性能实验

## 本次本地观测（2026-10-30，Windows）

原始记录见 [writer-windows-20261030.json](benchmarks/writer-windows-20261030.json)。
一次受控实验中：同步总耗时约 1016ms，异步总耗时约 1008ms；同步单次调用最大约 16.47ms，try_submit 最大约 9.1µs；队列峰值 65536 字节。
这是 Windows 睡眠粒度下的人工慢 sink 结果，主要验证提交与消费者等待分离及内存上限；不能把两种调用耗时之比称作程序加速倍数。


## 测什么

旧实现由监督线程同步 write 输出文件。新实现用受限 FIFO 和工作线程，把写入等待移到消费者线程；队列满时暂停管道读取。
优化目标是缩短监督线程被输出写盘阻塞的时间，并限制缓冲内存，**不承诺磁盘吞吐量增加**。
快磁盘、低输出负载下，线程同步和 eventfd 可能增加开销。

构建后执行：

```bash
./build/writer_benchmark
```

受控工作负载：64 个 4096 字节块；每次 sink 写入模拟等待 1ms。先测同步写入，再测有界后台写入（队列 64KiB）。
输出字段：

| 字段 | 含义 |
| --- | --- |
| sync_total_us / async_total_us | 同一工作量全部完成所需时间 |
| sync_max_call_ns | 同步调用一次 sink 的最大观测耗时 |
| async_max_submit_ns | 非阻塞 try_submit 的最大观测耗时，包含失败返回，不包含上层等待重试 |
| queue_peak_bytes | 队列加在写块的最大字节数 |
| backpressure_retries | 生产者因容量不足重试次数 |

该实验隔离“调用线程会不会等待 sink”这一因素，不等价于真实文件系统、真实取消延迟或生产性能。
Windows 的计时和睡眠粒度可能使 1ms 等待远大于 1ms；Linux 上需要重新采样。
一次最大值不是稳定的 p99 指标。不要从单次实验推断生产加速倍数。

## Linux 上进一步评估

固定内核、运行时、编译参数、资源限制和负载，对 0.4 与 0.5 分别测量：

- 无输出短任务：启动/结束开销。
- 大量输出任务：实际保存量、截断量、吞吐、queue_peak_bytes、backpressure_events。
- 输出期间发送取消：请求应答到任务回收的延迟分布，区分 SDK 启动 CLI 的开销。
- 多任务批次：max_parallel=1/2/4 的总完成时间、监督进程开销、实际资源预算占用。
- 慢存储场景：控制响应和最终关闭分别测量；write 卡住时 join 仍可能等待。

至少重复多轮报告中位数与尾部延迟，保留原始结果。同步状态保存、指标、日志也会影响取消延迟，不能全部归因于输出组件。

## 正确性优先的实验

writer_tests 使用条件变量 Gate 确定性阻塞 sink，验证正在写入的块仍计入容量、满队列不吞掉调用方数据、关闭后先排空再结束、短写保持顺序及故障传播。
output_runtime 测试验证 eventfd 完成、FD 整数复用、capture 截断和 /dev/full 写失败；需要真实 Linux。
batch_integration 检查并发/预算峰值、排队取消、运行取消、单项失败后的继续调度与输出完成。

ThreadSanitizer 尚未在当前交付环境运行；可在 Linux 单独给 writer_tests 和 sandbox_writer 添加 `-fsanitize=thread -g` 编译链接进行检查。不要把没有运行过的检测器写成已通过。
