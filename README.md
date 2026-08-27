# PresenceCardVenture · 小卡

拍照小卡（M5Stack CoreS3 Lite）+ 贴贴风云端社区。当前 MVP：1 张展示小卡，至少 200 位用户可扫码参与 Web 社区。

**团队先读：[MVP 分工（简版）](docs/05-team-roles.md)** —— 全栈 / 硬件 / 算法 / UI / CEO 的主责、交付物与共同验收。

> **规则第一条**：`docs/03-device-api.openapi.yaml` 是固件 / 后端 / web 三方**唯一接口契约**。
> 改契约 = 先提修改契约的 PR → 相关双方 review → 同步 `docs/02-device-api-v1.md` → merge。没有例外。
> 规范文档（02）与契约不一致时，以 02 为准并立即修契约。

## 仓库地图

| 目录 | 内容 | Owner |
|------|------|-------|
| `docs/` | 规划 / 架构 / API 规范 / OpenAPI 契约 / kickoff 手册 | 技术一号位 |
| `server/api/` | Fastify + Prisma API 服务 | 全栈 |
| `server/web/` | Vite + React 社区 web | 全栈（视觉稿来自 UI） |
| `server/deploy/` | docker-compose + Caddyfile + 备份脚本 | 全栈 |
| `firmware/` | PlatformIO ESP32-S3 固件 | 硬件 |

## 快速开始

```bash
# 契约 mock server（固件/前端零等待开工）
npx @stoplight/prism-cli mock docs/03-device-api.openapi.yaml

# API 服务
cd server/api && npm install && npm run dev     # → http://localhost:3000/health

# Web
cd server/web && npm install && npm run dev

# 固件
cd firmware && pio run

# 部署（ECS）
cd server/deploy && cp .env.example .env && docker compose up -d
```

## CI（.github/workflows）

| Workflow | 触发 | 作用 |
|----------|------|------|
| `contract` | 改契约/规范的 PR | OpenAPI YAML 校验 |
| `server-ci` | `server/**` 变更 | api 构建 + `prisma validate`，web 构建 |
| `firmware-ci` | `firmware/**` 变更 | PlatformIO 构建，产出 `firmware.bin` artifact（**技术一号位下载后在本地同款设备烧录复现**） |

## 文档索引（docs/）

00 总体规划 · 01 云端架构 · 02 设备 API 权威规范 · 03 OpenAPI 契约 · 04 kickoff 作战手册 · [05 当前 MVP 分工](docs/05-team-roles.md)
