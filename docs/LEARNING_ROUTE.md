# 0.5 学习路线：从 C++ 模型到 Linux 执行引擎

先读 TASK_ENGINE.md 形成完整路径，再按以下顺序学习。先看小测试，再看实现；每步写下所有者、状态变化、失败处理。

| 步骤 | 入口 | 要能解释的问题 |
| --- | --- | --- |
| 1 使用与报告 | README、SESSION_API、profiles/batch-process-demo.json | 单次、会话、批次有什么区别？process 为何不是隔离？ |
| 2 类型建模 | command_model.hpp/cpp、session_policy.hpp、task_tests.cpp | ID 为什么分类型？JSON 在哪里解析？optional/chrono 表达了什么约束？ |
| 3 RAII | linux.hpp、command_execution.hpp、session_runtime_tests.cpp | FD、管道、命令对象何时创建、移动、关闭？失败时谁回收？ |
| 4 状态与恢复 | session_state.hpp、session_store.cpp、session_contract/session_store 测试 | 为什么 EOF/进程结束/写入完成是不同事实？意图先落盘如何防止盲目重试？ |
| 5 线程与背压 | bounded_writer.hpp/cpp、writer_tests.cpp | condition_variable 为什么要谓词？队列容量为何包含在写块？拒绝入队为何不移动参数？ |
| 6 输出组件 | output_capture.cpp、output_runtime_tests.cpp | 正常 EOF 后如何结束消费者？短写、ENOSPC、队列满分别怎么处理？ |
| 7 事件等待 | event_loop.hpp/cpp | eventfd 如何通知监督器？FD 数字复用如何处理？为什么不保存捕获对象的回调？ |
| 8 连续会话 | interactive_session.cpp | 跟踪 ExecuteRequest → CommandExecution → COMMAND_DONE → EOF+flushed → result.json |
| 9 一次执行 | run_session.cpp | 与会话路径共享了什么？输出预算如何避免饿死控制/期限事件？ |
| 10 调度算法 | task.hpp/cpp、task_tests.cpp | deque FIFO 为什么会队首阻塞？预留/释放发生在哪一步？取消排队项是否消耗资源？ |
| 11 批次运行 | batch.cpp、batch_integration.py、batch_client.py | 为什么复用独立监督进程？为何 waitpid 后才释放预算？谁持有 pidfd？ |
| 12 Linux 隔离 | launcher_client、launcher/main、init/persistent_init、rootfs、security、cgroup | P0/P1/P2/P3 如何协作？namespace/cgroup/seccomp 各负责什么？ |
| 13 部署与测量 | launch.py、sandbox-run、delegated_run.py、deploy.py、PERFORMANCE.md | SDK/CLI 怎样委派？模拟慢写实验能证明什么，不能证明什么？ |
| 14 会话通信 | proto/session_v1.proto、session_wire.cpp、sdk/session_wire.py、run_store.cpp | 消息存在性、协议标识和 16 KiB 包边界如何保留旧业务语义？为什么要把编码变化和去掉子进程分别测量？ |

建议演示：

1. 执行两条连续命令，观察文件保留与独立命令结果。
2. 使用相同 request_id 重试，说明为什么得到同一 command_id。
3. 运行 writer_tests，用 Gate 解释满队列、短写和关闭先排空。
4. 运行 batch 示例，改变 max_parallel 和资源预算，观察 peak_active/peak_reserved_memory_bytes。
5. 取消一个 queued 任务和一个 running 任务，比较结果及资源释放。
6. 运行 writer_benchmark，解释总耗时近似不变而提交调用不必等待 sink 的原因。

先掌握一条正常路径，再讨论异常。不要一开始逐行阅读整个 batch.cpp 或整个隔离初始化。
暂不增加无锁队列、内存池或通用线程池；如果以后需要，先用测量说明现在的瓶颈。
