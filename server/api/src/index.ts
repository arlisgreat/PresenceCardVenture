import Fastify from 'fastify'

const app = Fastify({ logger: true })

app.get('/health', async () => ({
  status: 'ok',
  service: 'presence-card-api',
  version: '0.1.0',
}))

// ─────────────────────────────────────────────────────────────────
// TODO(全栈): 实现 v1 路由。
//   权威规范:   docs/02-device-api-v1.md
//   机器可读契约: docs/03-device-api.openapi.yaml
//   数据模型: prisma/schema.prisma
// 建议按契约拆模块：
//   app.register(import('./routes/pair.js'),    { prefix: '/v1/pair' })
//   app.register(import('./routes/photos.js'),  { prefix: '/v1/photos' })
//   app.register(import('./routes/feed.js'),    { prefix: '/v1/feed' })
//   app.register(import('./routes/friends.js'), { prefix: '/v1/friends' })
//   app.register(import('./routes/device.js'),  { prefix: '/v1/device' })
// ─────────────────────────────────────────────────────────────────

const port = Number(process.env.PORT ?? 3000)
app.listen({ port, host: '0.0.0.0' }).catch((err) => {
  app.log.error(err)
  process.exit(1)
})
