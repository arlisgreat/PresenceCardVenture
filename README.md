# PresenceCardVenture · 小卡

M5Stack CoreS3 Lite + Web 社区。`codex/fullstack` 当前提供可操作的全栈社区 Demo，并保留 ESP32-S3 设备 API 与生产适配层。

先读：[需求](docs/00-project-plan.md) · [分支与分工](docs/05-team-roles.md) · [文档索引](docs/README.md)

## 目录

| 目录 | 内容 |
|------|------|
| `docs/` | 需求、接口、交付 |
| `server/api/` | Fastify + Prisma API |
| `server/web/` | Vite + React Web |
| `server/deploy/` | Docker Compose + Caddy |
| `firmware/` | CoreS3 Lite 固件 |
| `presence-demo/` | 相框端动效演示 (纯前端单文件) |

## 开发

在仓库根目录运行：

```bash
npx @stoplight/prism-cli mock docs/03-device-api.openapi.yaml
npm --prefix server/api install
npm --prefix server/api run dev
npm --prefix server/web install
npm --prefix server/web run dev
pio run -d firmware
```

部署见[云端部署](docs/01-cloud-architecture.md)，烧录见[固件说明](firmware/README.md)。

## 相框端动效演示

`presence-demo/frame-motion-demo.html` 零依赖，双击浏览器打开即可。

- 画布 800×480，对应相框屏幕分辨率
- 完整剧本循环：待机 → 照片到达（遮罩变暗 + 拍立得飘入）→ 停留展示 → 点赞星星爆发 → 淡出回待机
- 所有动效时序集中在脚本顶部 `TIMING` 常量，移植到嵌入式时照搬数值即可
- 颜色 token、缓动曲线来自《Presence小卡-屏幕UI与交互量化设计规范》§1 / §4
## 本地完整 Demo

当前 `codex/fullstack` 分支包含可直接操作的社区应用 Demo：圈子时间线、我的足迹、三种玩法（轻美颜/CCD/素材模板）、图片上传与处理、模板贴纸、好友请求、文字/图片轻信号、AI 合照任务和设备模拟器。

```bash
# 终端 1：API
npm --prefix server/api install
npm --prefix server/api run dev

# 终端 2：Web
npm --prefix server/web install
npm --prefix server/web run dev -- --host 0.0.0.0 --port 5173
```

浏览器打开 `http://localhost:5173`。Demo API 使用预置身份 `demo-token`，无需真实账号或云端密钥即可走通主流程。上传图片会写入 `server/api/uploads/`，该目录已被 Git 忽略。

生产部署前需要替换 DemoStore、本地文件适配器和本地 AI 适配器，并注入 PostgreSQL、OSS、任务队列、HTTPS 域名和模型服务配置；初始 Prisma migration 已在 `server/api/prisma/migrations/0001_initial_schema/`，但 API 仍需接入真实 Prisma store adapter 后才能通过生产 readiness。设备端继续使用 `/v1` 契约。CI 会在 `main` 和 `codex/**` 分支自动执行 API 测试、构建和 Prisma schema 校验。
