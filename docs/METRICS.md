# 从性能监控项目吸收的设计与实际改动

本次参考 `monitor_system` 的统一接口、聚合器、单调时间采样与累计计数差分。没有复制整机指标、内核模块、eBPF、gRPC 或数据库部分。

## 对象关系

```text
RunSession
  ├─ CgroupV2：资源限制、归组、整体终止与删除
  └─ MetricsSampler：每运行独立生命周期
       └─ MetricRegistry
            ├─ cpu.stat / memory.events.local / pids.events
            ├─ memory.current / memory.peak / pids.current
            └─ io.stat 多设备计数
```

采集器负责读取和解析，sampler 负责时间与增量、汇总、历史，RunSession 决定时机并发布状态，RunReport 负责序列化结果。CgroupV2 不再每次临时创建默认采集器。

## 配置

```json
"metrics": {"interval_ms": 250, "history_limit": 256}
```

- interval_ms 范围 50..10000，默认250；不等于监督循环的20ms唤醒频率。
- history_limit 范围1..1024，默认256；仅保留最近样本，全程汇总仍保留。
- 不为新采集器启动线程，不执行 RPC 或外部命令。
- 在资源归组后、发 PREPARE 前建立基线；包含 init 环境准备的资源消耗。
- 收尾在删除 cgroup 前强制采集一次，保存内核峰值和 OOM 等事件。

## 指标口径

| 字段 | 来源或公式 |
|---|---|
| cpu_percent | delta(usage_usec) / delta(monotonic_ms) / 1000 × 100 |
| memory_current_bytes | 本实例 memory.current |
| memory_peak_bytes | 内核 memory.peak，不能用采样最大值代替 |
| pids_current | pids.current，包含线程及 init |
| read_bytes_per_sec | delta(io.stat 的各设备 rbytes 总和) / delta秒 |
| write_bytes_per_sec | delta(io.stat 的各设备 wbytes 总和) / delta秒 |

CPU 一个逻辑核=100%，并行可超过100%；不是配额归一化百分比。I/O 是块设备归账，缓存、tmpfs 写入未必产生块 I/O，不等于程序 write() 字节数。新设备出现等统计范围变化会影响总量增量，首版对总量回退进行重置处理，未提供热插拔设备的连续性保证。

## 实时查询与报告

```text
inspect 的 state.metrics.latest：当前样本（带相对采样基线的 elapsed_ms）
inspect 的 state.metrics.unavailable：当前采集失败字段
最终 metrics：原始末次计数和内核事件，保持既有字段
最终 metrics.sampling：历史、保留上限、丢弃数和全程观察汇总
```

字段缺失/文件损坏不影响其他采集器，值为null并记录原因。首次速率、相同时间点、缺少前值的速率为null。累计计数变小按counter_reset处理，不做无符号下溢。计数先用uint64做差，再转换double，避免长期累计值精度损失。

采集历史有界，没有全局静态样本表，不会串入其他运行的基线。process模式没有本实例cgroup，明确显示不可用，不去读取/proc/meminfo等整机来源充数。

## 能力与限制

- io控制器为可选观察能力；委派wrapper在控制器可用时启用，不可用时I/O字段null，其他指标继续。
- 采集不负责施加限制或停止目标，资源上限仍由内核执行。
- 状态快照更新有同步文件I/O，慢存储可能延迟监督循环；不能据此承诺硬实时取消。
- 最终强制样本可能与上一样本时间相同，此时速率为null，内核末次事件仍会刷新。
- 采样峰值是观测值，短峰可能漏采；内核memory.peak单独保存。

## 测试

新增纯C++跨平台metrics测试：伪cgroup目录、无换行末行、损坏字段、计数溢出、多个设备、长计数精度、首次/零时间/回退、两个实例独立、缺失/恢复、采样间隔、有界历史、峰值与JSON。

Linux隔离测试增加实时inspect和末次报告采样断言。真实Linux行为仍需在支持的Linux环境运行，跨平台测试与交叉编译不替代它。
