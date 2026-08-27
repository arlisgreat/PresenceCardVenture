# 云端部署

全栈负责阿里云部署；算法提供图像模块与配置。

| 组件 | 方案 / 目录 |
|------|-------------|
| Web | Vite + React；`server/web/` |
| API | Fastify + Prisma；`server/api/` |
| 数据库 | PostgreSQL；结构以 [schema.prisma](../server/api/prisma/schema.prisma) 为准 |
| 图片 | 阿里云 OSS 私有桶；鉴权后访问 |
| 部署 | Docker Compose + Caddy；`server/deploy/` |
| 图像处理 | 当前 Demo 使用本地可替换处理；生产接入算法服务适配器（`server/effects/`） |

- 开发、生产环境分离；交付 Web/API 地址、联调账号及配置模板。
- 上线前确认域名与 HTTPS；密钥仅在服务端注入。
- 设备图像规格与接口遵循 [02](02-device-api-v1.md)、[03](03-device-api.openapi.yaml)。
- 全栈交付日志、限流、每日备份、恢复步骤和压测报告。
- 算法交付效果配置、调用样例、超时/失败处理约定及成本评测。

当前 `codex/fullstack` 已提供社区时间线、好友请求、消息、AI 合照任务、照片上传/删除和设备联调接口。生产部署仍需将 DemoStore、本地文件存储和本地 AI 适配器替换为 PostgreSQL、OSS、任务队列及真实模型服务，并按本文件完成密钥、域名、备份和压测配置。

## 就绪探针

- `GET /health` 是存活探针，只表示 Fastify 进程可响应。
- `GET /health/ready` 是就绪探针；开发模式返回 `200` 并标注 `mode=demo`。
- 生产启动时设置 `REQUIRE_PRODUCTION_SERVICES=true`（或 `NODE_ENV=production`），就绪探针会要求 `DATABASE_URL`、`OSS_BUCKET`（或 `OBJECT_STORAGE_BUCKET`）和非模拟的 `AI_PROVIDER`，缺项返回 `503`，避免误把 DemoStore 部署为生产服务。
