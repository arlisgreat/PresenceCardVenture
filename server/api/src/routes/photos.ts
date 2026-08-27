import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody } from '../demo-store.js'
import { PhotoStore } from '../photo-store.js'

const user = (r: FastifyRequest, store: DemoStore) => store.userForToken(String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, ''))
export async function photoRoutes(app: FastifyInstance, opts: { store: DemoStore; files: PhotoStore }) {
  const { store, files } = opts
  app.post('/photos', async (r, reply) => {
    const u = user(r, store); if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'))
    const body = r.body as Buffer; if (!Buffer.isBuffer(body)) return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required'))
    if (body.length > 1024 * 1024) return reply.code(413).send(errorBody('PHOTO_TOO_LARGE','image exceeds 1048576 bytes'))
    if (body.length < 2 || body[0] !== 0xff || body[1] !== 0xd8) return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required'))
    const idem = String(r.headers['idempotency-key'] ?? ''); if (!idem) return reply.code(400).send(errorBody('BAD_REQUEST','Idempotency-Key required'))
    const deviceId = String(r.headers['x-device-id'] ?? 'web')
    const existing = [...store.photos.values()].find(p => p.authorId === u.id && p.idempotencyKey === idem && p.deviceId === deviceId)
    if (existing) return reply.code(200).send(uploadResult(existing, store))
    const usedToday = store.dailyUploadCount(u.id, deviceId)
    if (usedToday >= store.uploadDailyLimit) {
      const tomorrow = new Date(); tomorrow.setUTCHours(24, 0, 0, 0)
      return reply.code(429).send({ ...errorBody('RATE_LIMITED', 'daily upload limit reached'), retry_after: Math.max(1, Math.ceil((tomorrow.getTime() - Date.now()) / 1000)) })
    }
    const id = `p_${randomUUID()}`; const p = { id, authorId: u.id, filterId: String(r.headers['x-filter-id'] ?? 'none'), playType: String(r.headers['x-play-type'] ?? 'ccd'), beauty: Number(r.headers['x-beauty'] ?? 0), sticker: String(r.headers['x-sticker'] ?? 'none'), caption: r.headers['x-caption'] ? decodeURIComponent(String(r.headers['x-caption'])) : null, circle: String(r.headers['x-circle'] ?? '小圈'), width: Number(r.headers['x-width'] ?? 320), height: Number(r.headers['x-height'] ?? 240), createdAt: new Date().toISOString(), original: body, processed: body, idempotencyKey: idem, deviceId }
    store.photos.set(id, p); await files.save(p); return reply.code(201).send(uploadResult(p, store))
  })
  app.get('/photos/:id/image', async (r, reply) => { const p = store.photos.get((r.params as any).id); if (!p) return reply.code(404).send(errorBody('NOT_FOUND','photo not found')); reply.header('cache-control','public, max-age=31536000, immutable').type('image/jpeg').send(p.processed) })
  const feed = async (r: FastifyRequest, reply: any, mine = false) => { const u = user(r, store); if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); const photos = (mine ? [...store.photos.values()].filter(p=>p.authorId===u.id) : store.visiblePhotos(u.id)); const etag = `W/\"feed-${photos.map(p=>p.id).join('-')}\"`; if (r.headers['if-none-match'] === etag) return reply.code(304).send(); return reply.send({ items: photos.slice(0, Number((r.query as any)?.limit ?? 8)).map(p=>item(p, u.id, store)), next_cursor: null, etag }) }
  app.get('/feed', (r, reply) => feed(r, reply, false)); app.get('/photos/mine', (r, reply) => feed(r, reply, true))
  app.delete('/photos/:id', async (r, reply) => { const u = user(r, store), p = store.photos.get((r.params as any).id); if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); if (!p) return reply.code(404).send(errorBody('NOT_FOUND','photo not found')); if (p.authorId !== u.id) return reply.code(403).send(errorBody('FORBIDDEN','not owner')); store.photos.delete(p.id); await files.remove(p); return reply.code(204).send() })
}
function uploadResult(p: any, store?: DemoStore) { return { photo_id: p.id, url: `/v1/photos/${p.id}/image`, created_at: p.createdAt, daily_remaining: store ? Math.max(0, store.uploadDailyLimit - store.dailyUploadCount(p.authorId, p.deviceId ?? 'web')) : 60 } }
function item(p: any, uid: string, store: DemoStore) { const author = store.user(p.authorId); const my = Object.keys(store.reactionsFor(p.id)).filter(type => store.reactions.has(`${p.id}:${type}:${uid}`)); return { photo_id: p.id, author: { username: author?.username ?? '', display_name: author?.displayName ?? '' }, filter_id: p.filterId, play_type: p.playType ?? 'ccd', beauty: p.beauty ?? 0, sticker: p.sticker ?? 'none', caption: p.caption, circle: p.circle ?? '小圈', created_at: p.createdAt, width: p.width, height: p.height, image_url: `/v1/photos/${p.id}/image`, reactions: store.reactionsFor(p.id), my_reactions: my, mine: p.authorId === uid } }
