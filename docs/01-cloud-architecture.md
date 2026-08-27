# 小卡 MVP · 云端架构方案 v0.2（阿里云落地版）

> **v0.2 修订**：原 v0.1 推荐 Cloudflare 全家桶。现有全栈工程师 + 阿里云服务器与域名，
> 改为 **单台 ECS + 阿里云 OSS** 落地：国内访问与 ICP 问题消失、有人专职运维、成本同样≈0。
> **设备端 API 规范（02 文档）不受影响**——契约先行，底座可换。Cloudflare 方案降级为"海外版备选"。
>
> 面向：产品 / 全栈工程师 / 硬件工程师
> 范围：MVP（拍照 → 滤镜 → 上传 → community → 好友照片回显），100 用户

---

## 1. 产品阶段拆分（不变）

| 阶段 | 功能 | 云端依赖 |
|------|------|----------|
| **MVP（现在）** | 小卡拍照、滤镜、WiFi 上传、web community、好友照片回显小卡 | 全套 |
| **Phase 2** | 卡间不联网交友（ESP-NOW 碰一碰），联网后同步 | 复用好友/照片接口 + 离线 ticket |
| **Phase 3** | 广场、评论、群组、实时推送 | 加 Redis/WebSocket 或迁移 |

Feed 规则：**贴贴模式——只能看自己和好友的照片**，无公共广场。

---

## 2. 部署架构（阿里云）

```
小卡 ESP32-S3 ──HTTPS──┐
                        ├─► api.域名 ──► ECS（Docker Compose）
Web 社区  ────HTTPS──┘                  ├─ Caddy      ：TLS 自动证书 + 反代 + 限流
app.域名 ──────────────► 静态 SPA ────► ├─ api 服务    ：Node.js/TS（Fastify + Prisma）
                                        ├─ Postgres 16：业务数据（数据卷 + 每日备份→OSS）
                                        └─ （照片走 SDK 直传/直读阿里云 OSS，私有桶）
照片访问：GET /photos/{id}/image ──302──► OSS 预签名 URL（10 分钟过期）
备份：cron pg_dump → OSS（冷备，保留 30 天）
```

### 组件与建议技术栈

| 组件 | 建议 | 说明 |
|------|------|------|
| 反向代理/TLS | **Caddy**（或 Nginx + acme.sh） | Caddy 自动申请/续期 Let's Encrypt，配置 10 行；证书链对 ESP32 友好（ISRG Root X1） |
| API 服务 | **Node.js + TS + Fastify + Prisma** | 全栈工程师最熟的栈优先；Fastify 轻量、自带 schema 校验，与 OpenAPI 契约对齐容易 |
| 数据库 | **Postgres 16**（docker 数据卷） | 100 用户跑不满 1% 负载；同时是好友/feed/幂等键的唯一事实源 |
| 照片存储 | **阿里云 OSS，私有桶** | 标准存储 ¥0.12/GB/月 ≈ 免费；**私有桶 + 预签名 URL**，好友照片不可被公开遍历 |
| Web 前端 | React SPA（Vite 构建） | 构建产物由 Caddy 直接托管在 `app.域名`，与 API 同服务器 |
| 缓存/会话 | 不用 Redis | MVP 用 Postgres 存 session/计数即可，少一个组件少一份运维 |
| 部署 | docker compose + GitHub Actions（push main → ssh 部署） | 一台机器也要 CI，回滚 = 重打 tag |

> 原则：**单 ECS 一把梭，但每个组件容器化**；数据（Postgres 卷、OSS）与计算分离，以后换机器/加机器不丢东西。

### 域名与备案（关键路径，见 00 规划文档）

| 子域名 | 用途 |
|--------|------|
| `api.域名` | 设备 + web 共用 API（生产） |
| `api-dev.域名` | dev 联调环境（数据每周清空） |
| `app.域名` | web 社区 |

- **域名指向大陆 ECS 的 80/443 必须完成 ICP 备案**（若已有备案，新增网站/子域名一般直接可用）。
- 备案期间联调不受阻：ECS 公网 IP + 非标端口 / 内网穿透（frp、cloudflared tunnel）供开发用；**正式对 100 用户开放前必须备案完成**。
- 若服务器本来就在香港/海外：免备案，但国内访问延迟与稳定性打折，建议只在过渡方案中考虑。

---

## 3. 账户与设备绑定模型（不变）

配对码模型：设备领 6 位码显示 → 用户在 web 输入 → 设备轮询拿长效 `device_token`（存 NVS）。
详见 02 文档 §1。好友体系：注册生成唯一 6 位 `friend_code`，web 端输入对方码发请求，接受后双向可见。

---

## 4. 数据模型（Postgres）

```sql
CREATE TABLE users (
  id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  username      TEXT UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,               -- bcrypt/argon2
  display_name  TEXT NOT NULL,
  friend_code   TEXT UNIQUE NOT NULL,        -- 6 位，可刷新
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE devices (
  id              TEXT PRIMARY KEY,          -- 出厂唯一 id（MAC 派生 / 烧录 uuid）
  user_id         UUID REFERENCES users(id), -- NULL = 未绑定
  label           TEXT,
  pair_code       TEXT,
  pair_expires_at TIMESTAMPTZ,
  token_hash      TEXT,                      -- device_token 的 SHA-256，明文不落库
  fw_version      TEXT,
  last_seen_at    TIMESTAMPTZ,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- 上传幂等：同 (device, key) 重复提交返回首次结果
CREATE TABLE idempotency_keys (
  device_id  TEXT NOT NULL,
  key        TEXT NOT NULL,
  photo_id   UUID NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (device_id, key)
);

CREATE TABLE friendships (
  id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  requester_id UUID NOT NULL REFERENCES users(id),
  addressee_id UUID NOT NULL REFERENCES users(id),
  status       TEXT NOT NULL DEFAULT 'pending',   -- pending | accepted | blocked
  source       TEXT NOT NULL DEFAULT 'web',       -- web | offline_ticket（Phase 2）
  created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(requester_id, addressee_id)
);

CREATE TABLE photos (
  id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  author_id  UUID NOT NULL REFERENCES users(id),
  oss_key    TEXT NOT NULL,                  -- photos/{author_id}/{photo_id}.jpg
  filter_id  TEXT NOT NULL DEFAULT 'none',
  caption    TEXT,                           -- ≤ 140 字符
  width      INT NOT NULL,
  height     INT NOT NULL,
  size_bytes INT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_photos_author_time ON photos(author_id, created_at DESC);

CREATE TABLE reactions (
  photo_id   UUID NOT NULL REFERENCES photos(id),
  user_id    UUID NOT NULL REFERENCES users(id),
  type       TEXT NOT NULL,                  -- heart | thumbsup | wow
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (photo_id, user_id, type)
);

-- Phase 2 预留：离线加好友票（服务器预签发，见面时互相递交）
CREATE TABLE offline_tickets (
  ticket_id  TEXT PRIMARY KEY,
  owner_id   UUID NOT NULL REFERENCES users(id),
  payload    TEXT NOT NULL,                  -- HMAC 签名 payload
  claimed_by UUID REFERENCES users(id),
  expires_at TIMESTAMPTZ NOT NULL,
  claimed_at TIMESTAMPTZ
);
```

**Feed 查询**：`photos WHERE author_id IN (me ∪ accepted friends) ORDER BY created_at DESC`，
游标分页（`cursor = created_at + id`，避免 offset 漂移）；ETag = 好友最新照片时间戳哈希。

---

## 5. 照片规格与存储策略

| 决策 | 取值 | 理由 |
|------|------|------|
| 上传规格 | 320×240 JPEG，质量≈12，≤100KB（硬上限 1MB） | ESP32-S3 一帧 PSRAM 装下；家用 WiFi 3-8s；lo-fi 调性成立 |
| 存储 | OSS 私有桶，`photos/{author}/{id}.jpg` | 私有桶杜绝公开遍历；生命周期规则可后期加（如 2 年转归档） |
| 访问 | API `GET /photos/{id}/image` 鉴权后 **302 → 预签名 URL（10 分钟过期）** | 设备拉 feed 后立即下载，过期无影响；web 同理 |
| 缩略图 | MVP 不做，`?size=` 参数预留 | 设备间互拉同为 320×240；web 用 CSS 展示 |
| 量估算 | 100 用户 × 5 张/天 × 50KB ≈ 0.75GB/月 | OSS 一年 < ¥15 |

---

## 6. 成本估算（100 用户）

| 项目 | 用量 | 月费用 |
|------|------|--------|
| ECS（已有） | 2C4G 绰绰有余 | ¥0 增量 |
| OSS 存储 | 稳态 ~1GB | ≈ ¥0.2 |
| OSS 请求/流量 | 照片小而少 | ≈ ¥1 |
| 域名（已有） | — | ¥0 增量 |
| ICP 备案 | 免费（阿里云 App 提交） | ¥0 |
| **合计** | | **≈ ¥1–2/月** |

---

## 7. 安全与防刷（MVP 最小集）

- `device_token`：32B 随机，**服务端只存 SHA-256**；设备存 NVS（可开 flash 加密）。
- 配对码 10 分钟过期，错误 5 次锁 10 分钟。
- 上传限额 60 张/设备/天（429 + `retry_after`）；Caddy/应用层双重限流。
- OSS **私有桶 + 预签名**；web 上传入口剥 EXIF。
- 密码 bcrypt/argon2；web 会话 cookie HttpOnly + SameSite。
- 内容审核：好友闭环天然低风险；Phase 3 开广场前接阿里云内容安全。

---

## 8. Phase 2 预留：卡间不联网交友（设计不变）

设备在线时领 20 张服务器签名的一次性好友票（`offline_tickets`，HMAC）→ 见面经 ESP-NOW
互交 ticket + 名片照（320×240 分片传输，2–5s）→ 回家联网 `POST /friends/claim` 验签建好友。
MVP 只需：`friendships.source`、`offline_tickets` 表、`/friends/claim` 占位（先返回 501）。

---

## 9. 运维基线（单 ECS 也要做）

| 项 | 做法 |
|----|------|
| 备份 | 每日 cron：`pg_dump` → OSS（保留 30 天）；OSS 照片本身即持久化 |
| 监控 | docker 容器 restart=always；阿里云云监控（ECS CPU/磁盘告警，免费）|
| 日志 | API JSON 日志 → 文件 + logrotate；错误聚合（可选 Sentry 自托管先不做） |
| 告警 | 备份失败 / 磁盘 >80% / API 5xx 率 → 钉钉或企业微信 webhook |
| 重启手册 | 一页纸：compose up/down、恢复备份、重发证书——写给非运维也能照做 |

---

## 10. 路线图（与 00 项目规划对齐）

| 里程碑 | 内容 | 验收 |
|--------|------|------|
| M1（W1） | 服务器初始化 + dev 环境 + pairing/上传/feed 三接口 | curl 全流程脚本 + **真机 TLS 握手通过** |
| M2（W3） | 固件全链路 + web 基础 → 两台设备互见照片 | 02 文档 §7 验收用例全过 |
| M3（W4） | 内测加固：限额/备份/监控/管理极简后台 | 内测就绪评审 checklist |
| M4（W5+） | 10–20 人小范围 → 修 → 放开 100 人，稳定观察 2 周 | 崩溃率/失败率达标 |
| Phase 2 | ESP-NOW 离线交友 + ticket 兑换 | 无网环境演示 |
