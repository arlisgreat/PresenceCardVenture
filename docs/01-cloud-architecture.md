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
| AI 合照 | `server/api/src/ai-provider.ts` 定义 `AiProvider`；默认 `SimulatorAiProvider`，生产替换为真实模型适配器 |

- 开发、生产环境分离；交付 Web/API 地址、联调账号及配置模板。
- 上线前确认域名与 HTTPS；密钥仅在服务端注入。
- 设备图像规格与接口遵循 [02](02-device-api-v1.md)、[03](03-device-api.openapi.yaml)。
- 全栈交付日志、限流、每日备份、恢复步骤和压测报告。
- 算法交付效果配置、调用样例、超时/失败处理约定及成本评测。

当前 `codex/fullstack` 已提供社区时间线、好友请求、消息、AI 合照任务、照片上传/删除和设备联调接口。生产部署仍需将 DemoStore、本地文件存储和本地 AI 适配器替换为 PostgreSQL、OSS、任务队列及真实模型服务，并按本文件完成密钥、域名、备份和压测配置。

## 就绪探针

- `GET /health` 是存活探针，只表示 Fastify 进程可响应。
- `GET /health/ready` 是就绪探针；开发模式返回 `200` 并标注 `mode=demo`。
- API 每个响应都会返回 `X-Request-Id`（合法客户端 id 会被保留，否则由服务端生成），并附带 `X-Content-Type-Options: nosniff`、`X-Frame-Options: DENY` 与严格的 `Referrer-Policy`，便于链路排查并降低浏览器侧风险。
- 生产启动时设置 `REQUIRE_PRODUCTION_SERVICES=true`（或 `NODE_ENV=production`），就绪探针会要求 `DATABASE_URL`、`OSS_BUCKET`（或 `OBJECT_STORAGE_BUCKET`）、非模拟的 `AI_PROVIDER`、`PERSISTENCE_PROVIDER=prisma` 和 `DEVICE_TOKEN_ENCRYPTION_KEY`，并检查运行时实际注入的 store、会话及设备适配器；缺项返回 `503`，避免误把 DemoStore 部署为生产服务。当前仓库的照片、社交、AI 主 store 仍未切 Prisma，即使配置齐全也会因 `PRISMA_STORE_ADAPTER` 缺失而阻断。
- Web 会话解析已经通过 `UserSessionStore` 契约隔离：Demo 默认使用 `DemoSessionStore`，生产可注入 `PrismaSessionStore`。该适配器只用 bearer token 的 SHA-256 hash 查询 `Session`，并拒绝过期或撤销会话；生产 readiness 同时要求 `PRISMA_SESSION_ADAPTER`，避免主数据切换后认证仍落在 Demo 内存。
- 设备配对持久化切片已接入可选的 `devicePairStore` 路由边界：`PrismaDeviceStore` 对短期配对码做过期校验，绑定时保存 token hash 与服务端密文，`/pair/status` 可在重启后解密恢复设备 token。生产启动需设置 `DEVICE_TOKEN_ENCRYPTION_KEY`；未注入 Prisma 主 store 时 readiness 仍会阻断。
- Caddy 在 `APP_DOMAIN` 下将 `/v1/*` 同域反代到 API，并为 React Router 路径回退到 `index.html`；这样 Web 的相对 API 地址在开发和生产都保持一致。
- Compose 的 `deploy/.env` 同时注入 API 与 PostgreSQL；容器内 `DATABASE_URL` 必须使用 `db` 主机名，不能照搬本机 `localhost` 配置。
- Prisma 初始迁移已纳入 `server/api/prisma/migrations/0001_initial_schema/`，`docker compose exec api npx prisma migrate deploy` 现在有可执行的 schema 迁移；迁移完成不等于 API 已接入 Prisma store，`/health/ready` 仍会检查 adapter。

AI 合照任务只依赖 provider 契约：provider 返回 `completed`、`failed` 及结果照片 id，路由负责授权、排队状态和错误持久化。模拟器会返回第一张已授权素材作为可重复预览，不代表真实生图；接入第三方模型时通过 `buildApp({ aiProvider })` 注入，不改 Web 或设备 API。

照片路由同样只依赖 `PhotoStorage`（`save/read/remove`）契约。默认 `PhotoStore` 写入并读取本地目录供 Demo 使用；生产接入 OSS 时实现该接口并通过 `buildApp({ photoStorage })` 注入，不需要修改上传幂等、下载权限或删除逻辑。
