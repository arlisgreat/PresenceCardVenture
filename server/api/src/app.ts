import Fastify, { type FastifyInstance } from 'fastify'
import { randomInt, randomUUID } from 'node:crypto'
import path from 'node:path'
import { DemoStore, errorBody } from './demo-store.js'
import { PhotoStore, type PhotoStorage } from './photo-store.js'
import { photoRoutes } from './routes/photos.js'
import { socialRoutes } from './routes/social.js'
import { aiRoutes } from './routes/ai.js'
import type { AiProvider } from './ai-provider.js'

export async function buildApp(options: { uploadsDir?: string; store?: DemoStore; uploadDailyLimit?: number; requireProductionServices?: boolean; aiProvider?: AiProvider; photoStorage?: PhotoStorage } = {}): Promise<FastifyInstance> {
  const store = options.store ?? new DemoStore({ uploadDailyLimit: options.uploadDailyLimit }); const files = options.photoStorage ?? new PhotoStore(options.uploadsDir ?? path.resolve('uploads'))
  const requireProductionServices = options.requireProductionServices ?? (process.env.REQUIRE_PRODUCTION_SERVICES === 'true' || process.env.NODE_ENV === 'production')
  const app = Fastify({ logger: false, bodyLimit: 1024 * 1024 })
  app.addHook('onRequest', async (request, reply) => {
    const incoming = String(request.headers['x-request-id'] ?? '').trim()
    const requestId = /^[A-Za-z0-9._:-]{1,80}$/.test(incoming) ? incoming : randomUUID()
    reply.header('X-Request-Id', requestId)
    reply.header('X-Content-Type-Options', 'nosniff')
    reply.header('X-Frame-Options', 'DENY')
    reply.header('Referrer-Policy', 'strict-origin-when-cross-origin')
  })
  app.addContentTypeParser('image/jpeg', { parseAs: 'buffer' }, (_req, body, done) => done(null, body))
  app.setErrorHandler((err, _req, reply) => { const code=(err as any).code; if (code === 'FST_ERR_CTP_BODY_TOO_LARGE') return reply.code(413).send(errorBody('PHOTO_TOO_LARGE','image exceeds 1048576 bytes')); if (code === 'FST_ERR_CTP_INVALID_MEDIA_TYPE') return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required')); reply.send(err) })
  app.get('/health', async () => ({ status:'ok', service:'presence-card-api', version:'0.1.0' }))
  app.get('/health/ready', async (_request, reply) => {
    const aiProvider = String(process.env.AI_PROVIDER ?? '').trim().toLowerCase()
    const checks = {
      database: Boolean(process.env.DATABASE_URL),
      object_storage: Boolean(process.env.OSS_BUCKET || process.env.OBJECT_STORAGE_BUCKET),
      ai_provider: Boolean(aiProvider && !['demo', 'local', 'simulator'].includes(aiProvider)),
    }
    const missing = Object.entries(checks).filter(([, ok]) => !ok).map(([name]) => name === 'object_storage' ? 'OSS_BUCKET' : name === 'ai_provider' ? 'AI_PROVIDER' : 'DATABASE_URL')
    const ready = !requireProductionServices || missing.length === 0
    return reply.code(ready ? 200 : 503).send({ status: ready ? 'ready' : 'blocked', mode: requireProductionServices ? 'production' : 'demo', checks, missing })
  })
  app.get('/v1/me', async (r: any, reply) => {
    const token = String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, '')
    const user = store.userForToken(token)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    return { id: user.id, username: user.username, display_name: user.displayName, friend_code: user.friendCode }
  })
  app.post('/v1/pair/code', async (r:any,reply)=>{const {device_id}=r.body??{};if(!device_id)return reply.code(400).send(errorBody('BAD_REQUEST','device_id required'));let code='';do { code=String(randomInt(100000, 1000000)) } while ([...store.devices.values()].some(device => device.pairCode === code));const existing=store.devices.get(device_id);store.devices.set(device_id,{...existing,pairCode:code,expiresAt:Date.now()+600000});return {pair_code:code,expires_in:600}})
  app.get('/v1/pair/status', async (r:any,reply)=>{const d=store.devices.get(r.query.device_id);if(!d||d.pairCode!==r.query.pair_code)return reply.code(410).send(errorBody('PAIR_EXPIRED','pair code expired'));if(!d.userId)return reply.code(202).send({status:'pending'});return {status:'bound',device_token:d.token,user:{username:store.user(d.userId)?.username,display_name:store.user(d.userId)?.displayName}}})
  app.post('/v1/pair/bind', async (r:any,reply)=>{const u=store.userForToken(String(r.headers.authorization??'').replace(/^Bearer\s+/i,'')), body=r.body??{}, d=store.devices.get(body.device_id);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));if(!d||d.pairCode!==body.pair_code||!d.expiresAt||d.expiresAt<Date.now())return reply.code(410).send(errorBody('PAIR_EXPIRED','pair code expired'));d.userId=u.id;d.token=d.token??`device-token-${body.device_id}`;return {status:'bound',device_id:body.device_id,user:{username:u.username,display_name:u.displayName}}})
  app.post('/v1/device/config', async (r:any, reply) => {
    const token = String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, '')
    const user = store.userForToken(token)
    const body = r.body ?? {}
    const device = store.devices.get(body.device_id)
    if (!user) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    if (!device || !device.userId) return reply.code(404).send(errorBody('NOT_FOUND', 'device not found'))
    if (device.userId !== user.id) return reply.code(403).send(errorBody('FORBIDDEN', 'device is not owned by this user'))
    const idempotencyKey = String(r.headers['idempotency-key'] ?? '')
    if (idempotencyKey && device.configIdempotencyKey === idempotencyKey && device.configResponse) return reply.code(200).send(device.configResponse)
    const filterId = String(body.filter_id ?? '')
    const playType = String(body.play_type ?? '')
    const sticker = String(body.sticker ?? 'none')
    const beauty = Number(body.beauty ?? 0)
    if (!['none', 'warm', 'bw', 'film', 'vivid'].includes(filterId) || !['beauty', 'ccd', 'template'].includes(playType) || !['none', 'star', 'date'].includes(sticker) || !Number.isInteger(beauty) || beauty < 0 || beauty > 100) return reply.code(400).send(errorBody('BAD_REQUEST', 'invalid device config'))
    const config = { id: `cfg_${randomUUID()}`, filter_id: filterId, play_type: playType, beauty, sticker, updated_at: new Date().toISOString() }
    device.pendingConfig = config
    const response = { config_id: config.id, status: 'queued' as const, device_id: body.device_id, config }
    if (idempotencyKey) { device.configIdempotencyKey = idempotencyKey; device.configResponse = response }
    return reply.code(202).send(response)
  })
  app.register(async (scope)=>photoRoutes(scope,{store,files}),{prefix:'/v1'}); app.register(async scope=>socialRoutes(scope,store),{prefix:'/v1'}); app.register(async scope=>aiRoutes(scope,store,options.aiProvider,files),{prefix:'/v1'})
  app.get('/v1/device/state', async (r:any,reply)=>{const token=String(r.headers.authorization??'').replace(/^Bearer\s+/i,'');const user=store.userForToken(token);const device=[...store.devices.values()].find(d=>d.token===token);const owner=user ?? (device?.userId ? store.user(device.userId) : undefined);if(!owner)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const pending_friend_requests=[...store.friendRequests.values()].filter(x=>x.status==='pending'&&x.addresseeId===owner.id).length;return {unseen_count:store.visiblePhotos(owner.id).length,pending_friend_requests,server_time:new Date().toISOString(),fw_latest:null,pending_config:device?.pendingConfig ?? null,active_config:device?.activeConfig ?? null}})
  app.post('/v1/device/heartbeat', async (r:any,reply)=>{const token=String(r.headers.authorization??'').replace(/^Bearer\s+/i,'');const d=[...store.devices.values()].find(x=>x.token===token);if(!d)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));d.lastSeen=new Date().toISOString();return reply.code(204).send()})
  app.post('/v1/device/ack', async (r:any, reply) => { const token=String(r.headers.authorization??'').replace(/^Bearer\s+/i,''); const device=[...store.devices.values()].find(d=>d.token===token); if(!device)return reply.code(401).send(errorBody('TOKEN_INVALID','device token required')); const configId=(r.body as any)?.config_id; if(configId && device.pendingConfig?.id===configId) { device.activeConfig=device.pendingConfig; device.pendingConfig=undefined } return reply.code(204).send() })
  app.get('/v1/plays', async (_r) => ({ items: [{ id: 'beauty', name: '轻美颜', filters: ['soft'] }, { id: 'ccd', name: 'CCD 滤镜', filters: ['warm', 'bw', 'film', 'vivid'] }, { id: 'template', name: '素材模板', filters: ['none'] }] }))
  app.get('/v1/footprints', async (r:any, reply) => { const u=store.userForToken(String(r.headers.authorization??'').replace(/^Bearer\s+/i,'')); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); return [...store.photos.values()].filter(p=>p.authorId===u.id).map(p=>({date:p.createdAt.slice(0,10),photo_id:p.id,caption:p.caption,image_url:`/v1/photos/${p.id}/image`})) })
  await app.ready(); return app
}
