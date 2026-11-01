# 运行环境与权限边界

> 新增固定路径安装、应用专属授权、普通用户运行和卸载工具。请先阅读 [INSTALLATION.md](INSTALLATION.md)。
> 下面保留早期环境排查记录；“尚未实施安装工具”和“使用 unshare”的描述已被新实现取代。

## 本次已确认的状态

用户 Ubuntu 24.04.3 虚拟机的 8 项常规 CTest 已通过，isolation_integration 跳过。
显式委派隔离测试已尝试，但在 make_mounts_private 阶段返回 EACCES，目标程序未启动。
报告显示实例 cgroup 与后代已清理，不能将这次执行计为隔离通过。
该系统 apparmor_restrict_unprivileged_userns=1、unprivileged_userns_clone=1；尚无审计日志确认具体拒绝来源。
用户没有添加任何建议的 AppArmor 配置，本次项目修改也不会添加。

**后续（同一台机器、用户临时放开该开关之后）**：`sudo sysctl -w
kernel.apparmor_restrict_unprivileged_userns=0` 之后 `tools/delegated_run.py` 全绿——
`isolated integration passed`、`process sessions and artifact regression passed`、
`isolated session smoke passed`、`isolated /input snapshot passed`、
`isolated batch smoke passed`，服务以 `code=exited/status=0` 结束。
这说明此前的 EACCES 就是那个 AppArmor 开关造成的，而不是程序缺陷。
该开关不持久（重启即恢复默认），项目本身仍然不改宿主策略；要不要放开是使用方的决定。

## 先检查项目是否适合当前环境

```bash
./build/sandbox doctor
```

新增 isolation_probe，使用配置的 sandbox-launcher 启动独立探测进程。
依次尝试：新建 user/mount namespace → UID/GID 映射 → 将新命名空间挂载传播设为 private。
报告 available、stage、errno（适用时）、error 和探测进程的 AppArmor 上下文。
doctor 同时只读查询 AppArmor 限制开关、user namespace 开关和当前程序上下文。

available=false 表示当前这条隔离初始化路径被阻塞；不应继续把失败测试理解为程序运行失败。
available=true 只说明有限探测成功，不代表 rootfs、设备、PID namespace、seccomp、cgroup 或恶意代码隔离通过。
探测使用 unshare，实际任务用 clone3，两者仍可能受到不同系统调用策略；完整隔离测试不可省略。
缺失开关显示 null，不能据此推断该内核没有其他安全策略。

## 探测不会修改什么

- 不写 sysctl，不关闭 AppArmor，不创建或加载 /etc 下的策略。
- 不自动调用 sudo、不设置 setuid、不更改程序 file capabilities。
- 不修改宿主 mount namespace；挂载传播测试仅发生在成功新建的命名空间内。
- 不挂载 runtime、不执行用户命令、不准备任务 cgroup。
- 不退回 process 模式冒充隔离执行。

探测进程有三秒期限，结束后其临时 namespace 自行销毁。
delegated_run.py 在创建 supervisor/runs 子组前先做该检查；不支持时直接说明阶段并退出。
这一检查只排除已知阻塞，不能消除真正启动时策略变化或其他操作失败的可能性。

## 为什么不能通过代码忽略拒绝

本项目的 isolated 后端依赖内核授予新 user namespace 中所需的能力，以及 cgroup v2 委派。
允许创建 user namespace，不必然表示允许其内部的挂载操作。
Ubuntu 24.04 官方发行说明描述了：默认策略允许创建 namespace，但限制后续能力使用的情况。
参考：https://documentation.ubuntu.com/release-notes/24.04/ ，Unprivileged user namespace restrictions。

如果系统策略拒绝这些能力，省略检查、跳过挂载或自动提权都会改变已承诺的安全边界。
因此本次修复诊断、前置检查和部署说明，不声称绕过了该限制或修复了 Linux 隔离行为。

## 后续部署选择（尚未自动实施）

1. **普通用户部署**：管理员核实策略，批准特定安装路径的权限与 cgroup 委派。项目需要进一步提供受保护安装目录、安装/卸载和经过验证的策略。目前不把开发目录下的 unconfined 配置当成正式部署。
2. **管理员运行的专用验证环境**：管理员明确授权后，在独立 VM 使用受管理的服务运行。不能自动提权，也不能把 root 验证成功当作普通用户模式通过。
3. **当前环境不批准所需能力**：保留 process 模式作为可信命令调试，并明确它没有隔离。真正隔离需要另选获批准的执行环境；当前没有虚拟机执行后端。

本项目目前仍是原型，正式用户安装方案尚待设计、实现和 Linux 验证。
用户应能够在运行前看到支持条件，不应靠遇到一个错误就放宽一个全局限制。
