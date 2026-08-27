import Fastify, { type FastifyInstance } from 'fastify'
import path from 'node:path'
import { DemoStore, errorBody } from './demo-store.js'
import { PhotoStore } from './photo-store.js'
import { photoRoutes } from './routes/photos.js'
import { socialRoutes } from './routes/social.js'
import { aiRoutes } from './routes/ai.js'

export async function buildApp(options: { uploadsDir?: string; store?: DemoStore } = {}): Promise<FastifyInstance> {
  const store = options.store ?? new DemoStore(); const files = new PhotoStore(options.uploadsDir ?? path.resolve('uploads'))
  const app = Fastify({ logger: false, bodyLimit: 1024 * 1024 })
  app.addContentTypeParser('image/jpeg', { parseAs: 'buffer' }, (_req, body, done) => done(null, body))
  app.setErrorHandler((err, _req, reply) => { const code=(err as any).code; if (code === 'FST_ERR_CTP_BODY_TOO_LARGE') return reply.code(413).send(errorBody('PHOTO_TOO_LARGE','image exceeds 1048576 bytes')); if (code === 'FST_ERR_CTP_INVALID_MEDIA_TYPE') return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required')); reply.send(err) })
  app.get('/health', async () => ({ status:'ok', service:'presence-card-api', version:'0.1.0' }))
  app.post('/v1/pair/code', async (r:any)=>{const {device_id,fw_version}=r.body??{};if(!device_id)return {error:{code:'BAD_REQUEST',message:'device_id required'}};const code='482913';store.devices.set(device_id,{pairCode:code,expiresAt:Date.now()+600000});return {pair_code:code,expires_in:600}})
  app.get('/v1/pair/status', async (r:any,reply)=>{const d=store.devices.get(r.query.device_id);if(!d||d.pairCode!==r.query.pair_code)return reply.code(410).send(errorBody('PAIR_EXPIRED','pair code expired'));if(!d.userId)return reply.code(202).send({status:'pending'});return {status:'bound',device_token:d.token,user:{username:store.user(d.userId)?.username,display_name:store.user(d.userId)?.displayName}}})
  app.register(async (scope)=>photoRoutes(scope,{store,files}),{prefix:'/v1'}); app.register(async scope=>socialRoutes(scope,store),{prefix:'/v1'}); app.register(async scope=>aiRoutes(scope,store),{prefix:'/v1'})
  app.get('/v1/device/state', async (r:any,reply)=>{const token=String(r.headers.authorization??'').replace(/^Bearer\s+/i,'');if(!store.userForToken(token)&&![...store.devices.values()].some(d=>d.token===token))return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));return {unseen_count:store.photos.size,pending_friend_requests:0,server_time:new Date().toISOString(),fw_latest:null}})
  app.post('/v1/device/heartbeat', async (r:any,reply)=>{const token=String(r.headers.authorization??'').replace(/^Bearer\s+/i,'');const d=[...store.devices.values()].find(x=>x.token===token);if(!d)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));d.lastSeen=new Date().toISOString();return reply.code(204).send()})
  app.post('/v1/device/ack', async (_r, reply) => reply.code(204).send())
  app.get('/v1/plays', async (_r) => ({ items: [{ id: 'beauty', name: '轻美颜', filters: ['soft'] }, { id: 'ccd', name: 'CCD 滤镜', filters: ['warm', 'bw', 'film', 'vivid'] }, { id: 'template', name: '素材模板', filters: ['none'] }] }))
  app.get('/v1/footprints', async (r:any, reply) => { const u=store.userForToken(String(r.headers.authorization??'').replace(/^Bearer\s+/i,'')); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); return [...store.photos.values()].filter(p=>p.authorId===u.id).map(p=>({date:p.createdAt.slice(0,10),photo_id:p.id,caption:p.caption,image_url:`/v1/photos/${p.id}/image`})) })
  await app.ready(); return app
}
