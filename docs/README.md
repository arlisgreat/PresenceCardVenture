# docs/ — 契约与规划文档包

> **本目录规则第一条**：`03-device-api.openapi.yaml` 是固件/后端/web 三方唯一接口契约。
> 改契约 = 先提修改本文件的 PR，相关双方 review 通过，再动实现。任何"先改了再说"当场打回。
> 规范文档（02）与契约不一致时，以 02 为准并立即修契约。

## 文档索引

| 文件 | 内容 | 谁必读 |
|------|------|--------|
| [05-team-roles.md](05-team-roles.md) | **当前 MVP 分工（简版）**：五个角色的主责、交付与共同验收 | **全员先读** |
| `00-project-plan.md` | 分工、关键路径（ICP 备案）、五周计划、风险登记 | 全员 |
| `01-cloud-architecture.md` | 阿里云架构、数据模型（Prisma 实现在 `server/api/prisma/`）、成本、运维基线 | 全栈 |
| `02-device-api-v1.md` | **设备端 API 权威规范** + 固件实现 checklist + 验收用例 | 硬件、全栈 |
| `03-device-api.openapi.yaml` | 机器可读契约（Postman/Prism mock 直接导入） | 硬件、全栈 |
| `04-kickoff-playbook.md` | kickoff 议程、W1 任务卡、remote 协作协议、验收锚点 | 技术一号位 |

## 快速开始

```bash
# 起一个契约 mock server（固件/前端零等待开工）
npx @stoplight/prism-cli mock docs/03-device-api.openapi.yaml
```
