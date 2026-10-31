# 第三方依赖

- nlohmann/json **v3.11.3**，MIT，官方单头文件随源码提供，版权和许可见 `third_party/nlohmann/json.hpp`。
  来源：https://github.com/nlohmann/json/tree/v3.11.3
- spdlog **v1.14.1**，MIT，源码与 LICENSE 随工程提供。
  来源：https://github.com/gabime/spdlog/tree/v1.14.1
- libseccomp：Linux 构建使用系统开发包，使用其公开 API；许可与分发遵循安装版本的 LGPL 条款。本项目交付源代码不包含测试工具目录中的 Ubuntu 二进制库。
  来源：https://github.com/seccomp/libseccomp

`work/` 中 Zig、clang-format 和 Ubuntu 开发包仅用于本次验证，不进入源码交付包，也不作为项目运行时依赖。
