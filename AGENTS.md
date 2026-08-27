# AGENTS — PresenceCardVenture

- Monorepo：`docs/`（契约与规划）· `server/api`（Fastify+Prisma）· `server/web`（Vite+React）· `server/deploy`（compose+Caddy）· `firmware`（PlatformIO ESP32-S3）。
- 规则第一条：`docs/03-device-api.openapi.yaml` 是唯一接口契约；改契约先 PR；规范以 `docs/02-device-api-v1.md` 为准。
- 里程碑与验收标准：`docs/00-project-plan.md`、`docs/04-kickoff-playbook.md`。
- 设备端实现 checklist 与验收用例：`docs/02-device-api-v1.md` §6/§7。
