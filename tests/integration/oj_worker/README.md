# Python 评测 Worker 接入测试

这是一组真实 CLI 集成测试，不是完整 OJ。输入是仓库内固定的学生程序；Worker
通过项目 `sdk.launch.plan_launch` 启动隔离执行，使用 `stdin_file` 接入测试输入，
在沙箱外比较输出。预期答案不会写入提交程序可见的目录。

## 覆盖

七次独立运行：正确答案、错误答案、异常退出、死循环超时、输出截断、残留后代
清理、故障后的正常提交。输出超限案例故意让保存前缀与预期匹配，防止误判通过。
每次检查 EOF、写入完成、后代回收、launcher 回收和 cgroup 移除。
隔离失败、启动失败、清理不完整均判 SYSTEM_ERROR，终止测试；不回退 process。
超时优先于输出超限，比较规则为按空白分词比较。本测试不验证公平 CPU 计时、
OOM 实机行为、并发调度或对恶意代码的完整防御。

## Linux 实机执行

需要已经构建的 sandbox / sandbox-launcher、已验证的隔离环境，以及包含
Python 解释器、标准库、动态链接器和依赖库的 runtime_root。
项目静态 fixture runtime 不能直接运行 Python。

准备一份可工作的 isolated JSON 配置。脚本保留 runtime_root、cgroup_root 等
基础配置，但覆盖命令、挂载、stdin、网络与测试资源限制。不要用生产环境配置。
若使用 SDK 自动申请 systemd 用户委派，删除配置中的 cgroup_root；若指定此字段，
必须提供真实且已正确委派的 cgroup 根。以具备项目运行权限的普通用户执行。

在仓库根目录运行（替换配置路径）：

```bash
python3 tests/integration/oj_worker/oj_integration.py \
  --repo "$PWD" --binary "$PWD/build/sandbox" \
  --config /absolute/path/python-isolated.json \
  --python /usr/bin/python3 --results /tmp/oj-proof-01
```

results 必须是尚不存在的短路径，避免 UNIX socket 路径过长。每次运行保留配置、
CLI stdout/stderr、完整报告、输出文件及汇总 results.json，便于定位问题。
外部 40 秒 watchdog 超时使用 SDK launch plan 停止直接进程或 systemd 单元；
这种故障不会作为普通学生超时通过。

## 可移植判题逻辑检查

```bash
python3 -m unittest discover -s tests/integration/oj_worker -p test_judge.py -v
```

这些单元测试仅验证结果映射，不能证明 Linux 隔离有效。只有七次真实运行全部
通过，才会输出 `PASS: 7 real sandbox runs`。
