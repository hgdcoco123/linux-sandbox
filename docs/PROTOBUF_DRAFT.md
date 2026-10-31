# 会话 Protobuf 协议与验证

文件名保留自早期草案。`proto/session_v1.proto` 定义外部会话协议：每个 UNIX SOCK_SEQPACKET 数据包以 `SBP1` 开头，随后是一个 Protobuf 消息；原 JSON 包与单次运行的原始 `cancel` 仍受支持。客户端和服务端各自限制包长度为 16 KiB，拒绝截断及附带控制数据。服务端校验连接 UID，SDK 校验状态目录所有者和权限，并校验对端 UID。

wire 版本为 1，独立于旧 JSON 会话应用协议版本 1/2 和持久化格式。请求的 oneof 覆盖执行、状态、取消、关闭、输入、输出、结果、请求查询与工作区；响应按命令、状态、确认、输入、输出、工作区和错误分类。字段编号不复用，删除字段应保留编号。执行请求的 `optional` 和环境包装保留缺省与空值；输入输出是原始字节。命令结果用字段存在性标记区分缺失、null 和数字零；pending 只含 state，状态摘要可省略 argv/cwd。仅动态 metrics 子树以 UTF-8 JSON 保存于专用扩展字段，避免把 64 位整数转成浮点。

JSON 与 Protobuf 在控制层转换成同一应用请求，去重仍比较业务请求，接受记录先持久化。运行中的 SDK 普通请求直接连接；关闭后只读 request-status 可由 CLI 核验并查询记录。响应丢失后 SDK 不重发 execute、stdin 或其他有副作用的操作；调用者可用 request_id 查询。批次控制独立使用原 JSON 路径；内部 launcher/init 协议和 FD 传递不变。

CMake 在构建时用系统 protoc 生成 C++，Python 生成文件随 SDK 发布；修改 proto 后须重新生成 Python 文件。安装时由用户准备匹配的系统 Protobuf C++ 开发包和 Python 运行库，不自动下载依赖。升级前关闭活动会话并同时升级 CLI、监督进程和 SDK；恢复旧版本需同时回退它们。新客户端连接旧服务端会明确失败，不会因传输失败自动切换协议重发有副作用请求。

本机 C++/Python 编解码、边界和交叉链接结果见 `VALIDATION.md`。本轮真实 Linux、正式安装、隔离与性能测量须另行执行，历史版本通过结果不替代新版本验收。`tools/session_protocol_benchmark.py` 可对空闲会话分别测量旧 CLI/JSON、Python 直连 JSON、Python 直连 Protobuf，`--legacy-binary` 必须指向仍发送 JSON 的旧版 CLI；A 到 B 包含去掉子进程的收益，B 到 C 才更接近编码和相应适配开销的差异。
