# 安全说明

这个项目执行**不可信代码**。请先读 [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md)：它列了防什么、
不防什么，以及每条声明对应的测试。

## 汇报漏洞

请**不要**开公开 issue。发邮件给维护者（仓库里的提交邮箱），内容包括：

- 受影响的版本（`sandbox --version`）与内核/发行版；
- 复现用的配置（`config.json`）与目标程序；
- 你观察到的报告字段（`verdict`/`summary`/`errors`/`workspace.export`）；
- 如果是隔离逃逸，说明你从沙箱里访问到了什么。

已知**不在范围内**的（不是漏洞，是设计前提）：内核 0day、侧信道、多租户隔离（本项目是单用户工具）、
`process` 模式（`--allow-process`/`--process` 明确不是安全边界）、`checksums.json` 被连哈希一起伪造
（自证，不防伪造）。

## 升级建议

- 报告字段的兼容承诺见 [docs/CONTRACT.md](docs/CONTRACT.md)；
- 新版发布见 [docs/RELEASING.md](docs/RELEASING.md)，每个 Release 的 notes 会写清"已验证 / 未验证"。
