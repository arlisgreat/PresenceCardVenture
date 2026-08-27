# PresenceCardVenture · 小卡

M5Stack CoreS3 Lite + Web 社区。当前为工程骨架，功能待实现。

先读：[需求](docs/00-project-plan.md) · [分支与分工](docs/05-team-roles.md) · [文档索引](docs/README.md)

## 目录

| 目录 | 内容 |
|------|------|
| `docs/` | 需求、接口、交付 |
| `server/api/` | Fastify + Prisma API |
| `server/web/` | Vite + React Web |
| `server/deploy/` | Docker Compose + Caddy |
| `firmware/` | CoreS3 Lite 固件 |

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
