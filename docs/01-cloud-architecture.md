# 云端部署

全栈负责阿里云部署；算法提供图像模块与配置。

| 组件 | 方案 / 目录 |
|------|-------------|
| Web | Vite + React；`server/web/` |
| API | Fastify + Prisma；`server/api/` |
| 数据库 | PostgreSQL；结构以 [schema.prisma](../server/api/prisma/schema.prisma) 为准 |
| 图片 | 阿里云 OSS 私有桶；鉴权后访问 |
| 部署 | Docker Compose + Caddy；`server/deploy/` |
| 图像处理 | 算法模块；`server/effects/`（待建） |

- 开发、生产环境分离；交付 Web/API 地址、联调账号及配置模板。
- 上线前确认域名与 HTTPS；密钥仅在服务端注入。
- 设备图像规格与接口遵循 [02](02-device-api-v1.md)、[03](03-device-api.openapi.yaml)。
- 全栈交付日志、限流、每日备份、恢复步骤和压测报告。
- 算法交付效果配置、调用样例、超时/失败处理约定及成本评测。

当前业务接口待实现；数据模型、聊天、生成任务及下发回执按需求补齐。
