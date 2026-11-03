# 固定路径安装与普通用户运行

> 0.5 新增 `sandbox-run --batch --config FILE`，不变更应用专属 AppArmor 策略；完整验收新增批次和输出线程，见 VALIDATION.md。

> 0.4：安装策略不变。普通用户 CLI 包装器与 Python SDK 共用 sdk/launch.py；升级载荷会包含该模块。
> SDK 自动委派路径应额外执行 tests/acceptance/installed_sdk.py 验收，步骤见 VALIDATION.md。

> 最新结果：用户已在上述 Ubuntu 环境完成安装、普通用户探测、保留原策略升级及完整隔离脚本验收；详见 VALIDATION.md 最新记录。真实卸载和全面故障注入仍待验证。

## 1. 目的与安全边界

Ubuntu 24.04 对非特权 user namespace 有 AppArmor 限制。用户反馈已显示启动器进入
`unprivileged_userns (enforce)`，映射或挂载操作被拒绝。
本次把所需授权整理为可审阅、可卸载的正式步骤，不通过忽略错误来假装隔离成功。

- 管理员一次性安装可信构建产物并批准应用策略。
- 普通用户运行，不设置 setuid/file capabilities，不安装 root 常驻服务。
- 不写 sysctl，不关闭全局 AppArmor，不更改其他应用策略或全局 cgroup 设置。
- 默认目标：Ubuntu 24.04、AppArmor 4.x、systemd 用户会话、cgroup v2。
- 真实安装与隔离测试尚待 Linux 验收；本版不是已审计的多租户安全产品。

## 2. 安装布局

```text
/opt/linux-sandbox/
  bin/{sandbox,sandbox-launcher,sandbox-run}
  share/linux-sandbox/{tools,sdk,tests,profiles,docs,packaging}
  INSTALL_MANIFEST.json
  INSTALLED_APPARMOR
/etc/apparmor.d/linux-sandbox-installed-launcher
```

安装文件归 root 所有；目录和执行文件 0755，数据文件 0644。普通用户不能替换启动器。
前缀固定，避免策略匹配路径与实际程序不一致。不自动创建 PATH 链接或修改登录配置。
安装源须由管理员信任并冻结，不能使用不可信租户提交的构建文件。

## 3. 策略授权范围

策略文件为 packaging/apparmor/linux-sandbox-installed-launcher，路径只匹配：

```text
/opt/linux-sandbox/bin/sandbox-launcher
```

它采用 Ubuntu 官方说明的 flags=(unconfined) 与 userns 兼容授权。
**它不是完整的 AppArmor 限权策略**，而是批准该程序使用 namespace 能力、自行建立沙箱。
这是明确的应用级安全授权，须由管理员批准。配置提交者仍需可信。
开发目录 build/sandbox-launcher 不受该策略授权；复制到 /opt 之外运行仍可能失败。

依据：

- https://documentation.ubuntu.com/release-notes/24.04/ ，Unprivileged user namespace restrictions。
- https://manpages.ubuntu.com/manpages/noble/man8/apparmor_parser.8.html ，策略检查/加载/卸载。
- https://systemd.io/CGROUP_DELEGATION/ ，服务级 cgroup 委派。

若管理员不批准所需能力，当前后端不能绕过拒绝；不会自动降级或提权。

## 4. 构建和审阅（普通用户）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
python3 tools/deploy.py plan --build-dir build > install-plan.json
less install-plan.json
```

plan 不改变系统，输出目标文件、权限、SHA-256 和完整策略内容。
输入二进制须具有 ELF 标识；这不代替对构建来源的信任。
可选的普通目录暂存：

```bash
python3 tools/deploy.py stage --build-dir build --destination ./package-preview
```

目标须不存在。stage 不加载策略。Windows 可测试载荷逻辑，不能验证 Linux 权限。

## 5. 管理员明确安装

审阅计划后才执行以下一次性系统变更：

```bash
sudo /usr/bin/python3 tools/deploy.py install --build-dir build --apply
```

这会写 /opt/linux-sandbox、该程序的 /etc/apparmor.d 策略并加载它。
不会替用户运行任务，也不会把 root 安装成功当成普通用户隔离成功。

安装器检查受保护的祖先目录、符号链接、已有安装/策略，持有专属部署锁。
先暂存载荷和清单，再用 apparmor_parser -Q -K 检查策略，最后发布并加载。
策略加载失败时回滚本次文件；若策略卸载失败则保留安装并报错，不掩盖错误。
已存在的安装不会被 install 覆盖；同策略更新请用下述 upgrade。

跨文件系统状态和内核策略不构成原子事务。掉电/SIGKILL/磁盘故障可能留下
/opt/.linux-sandbox-install-*；需管理员核查，不递归删除未知内容。

## 6. 用普通用户验收

不要给下面命令加 sudo：

```bash
/opt/linux-sandbox/bin/sandbox doctor
```

检查 uid 为普通用户、launcher 指向固定安装路径、isolation_probe.available=true。
AppArmor 全局限制开关可以保持 1。若仍失败，请保留完整诊断。

探测现在使用 clone3 创建 user/mount/PID/IPC/UTS/network namespace，由父进程写 UID/GID 映射，
再由子进程测试私有挂载，与默认禁网任务基本创建方式一致。
它不挂载 runtime、不配置任务 cgroup、不执行目标；通过仍不代表完整隔离验收。
临时命名空间退出后销毁，不修改宿主挂载。探测有三秒期限。

准备可信 fixture runtime，并运行安装后的完整测试：

```bash
test_root=$(mktemp -d /tmp/sbx-installed.XXXXXX)
python3 /opt/linux-sandbox/share/linux-sandbox/tools/make_runtime.py \
  --fixture "$PWD/build/sandbox-fixture" --output "$test_root/runtime"
/opt/linux-sandbox/bin/sandbox-run --test-runtime "$test_root/runtime"
```

sandbox-run 调用当前用户的 systemd-run --user，为本次服务申请 Delegate=yes。
委派脚本只在这个服务的 cgroup 下建立 supervisor/runs 子树。
用户 manager 不提供所需 cpu/memory/pids 控制器时明确失败，不修改全局 user@.service。
上述入口使用已安装测试和 SDK，runtime 来源仍为用户指定的可信 fixture。

## 7. 运行自己的任务

```bash
/opt/linux-sandbox/bin/sandbox-run --config /绝对路径/job.json \
  --state-root /tmp/my-sandbox-state
```

入口只接受 isolated 配置，根据 session.enabled 选择 run 或 session-open。
`--print-command` 仅打印将要执行的参数数组。所有路径按参数传递，不拼接 shell 命令。
普通用户入口拒绝 root 调用，不安装全局任务服务，不新增多实例调度或 C++ SDK。

连续会话的监督进程在用户服务中存活，其他客户端用 state-root/run_id 请求或 attach。
原 Python SessionClient.create 直接启动 CLI，不会替调用方申请 cgroup 委派；
它仍需在已有委派环境使用，也可 attach 到 sandbox-run 创建的会话。
本次不承诺 SDK 在任意机器上自动获得完整隔离。

## 8. 检查与卸载

### 保持原授权升级代码

停止提交任务并关闭全部会话，在新源码目录构建后运行：

```bash
sudo /usr/bin/python3 tools/deploy.py upgrade --build-dir build --apply
```

只更新 /opt/linux-sandbox 载荷和清单；要求现有策略、新策略模板及安装记录逐字一致。
不会加载、卸载或重写 AppArmor 策略，不增加权限，不触碰用户数据。
升级检查原安装完整性和活动进程，暂存后交换目录，发布失败尝试恢复旧载荷。
维护期间勿并发启动直接 CLI；目录交换不是单步原子事务，掉电可能需要管理员恢复。

### 检查与卸载

```bash
python3 /opt/linux-sandbox/share/linux-sandbox/tools/deploy.py status
```

检查载荷摘要、权限、归属和策略文件是否匹配。普通用户无法读取内核策略列表时，profile_loaded=null。
status 不修改或修复文件。

关闭所有会话并停止新任务后，管理员执行：

```bash
sudo /usr/bin/python3 /opt/linux-sandbox/share/linux-sandbox/tools/deploy.py uninstall --apply
```

文件被改动、有未知文件、出现符号链接或检测到活动 sandbox 进程时拒绝卸载。
卸载只移除该项目策略和清单内文件，不删除用户 runtime、输入、输出或历史记录。
不提供强制删除未知文件选项。维护时勿并发启动直接 CLI；进程扫描不是全局调度锁。

## 9. 实际验证情况

14 项部署模拟测试通过：只读计划、暂存清单、篡改拒绝、路径规则、用户入口、模拟策略加载失败回滚等。
这些测试没有加载真实 AppArmor，也没有修改当前虚拟机。
Ubuntu 实际策略加载、普通用户 doctor、委派隔离测试和实际卸载尚待验收。
安装成功不代表生产安全，失败应保留诊断，不能通过放宽全局限制冒充通过。
