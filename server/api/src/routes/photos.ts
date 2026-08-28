import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody } from '../demo-store.js'
import type { PhotoStorage } from '../photo-store.js'
import type { DevicePairStore } from '../prisma-device-store.js'
import type { PhotoMetadataRepository } from '../prisma-photo-repository.js'
import { renderPhoto } from '@pvc/effects'

const auth = async (r: FastifyRequest, store: DemoStore, devicePairStore?: DevicePairStore) => {
  const token = String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, '')
  const device = store.deviceForToken(token) ?? (devicePairStore ? await devicePairStore.deviceForToken(token) : undefined)
  const persistentUser = device && 'user' in device ? device.user : undefined
  const account = store.userForToken(token) ?? (device?.userId ? store.user(device.userId) : undefined) ?? persistentUser
  return { account, device }
}
export async function photoRoutes(app: FastifyInstance, opts: { store: DemoStore; files: PhotoStorage; devicePairStore?: DevicePairStore; photoMetadataRepository?: PhotoMetadataRepository }) {
  const { store, files, devicePairStore, photoMetadataRepository } = opts
  app.post('/photos', async (r, reply) => {
    const { account: u, device } = await auth(r, store, devicePairStore); if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'))
    const body = r.body as Buffer; if (!Buffer.isBuffer(body)) return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required'))
    if (body.length > 1024 * 1024) return reply.code(413).send(errorBody('PHOTO_TOO_LARGE','image exceeds 1048576 bytes'))
    if (body.length < 2 || body[0] !== 0xff || body[1] !== 0xd8) return reply.code(415).send(errorBody('BAD_CONTENT_TYPE','JPEG required'))
    const idem = String(r.headers['idempotency-key'] ?? ''); if (!idem) return reply.code(400).send(errorBody('BAD_REQUEST','Idempotency-Key required'))
    const deviceId = String(r.headers['x-device-id'] ?? 'web')
    if (device && device.id !== deviceId) return reply.code(403).send(errorBody('FORBIDDEN', 'device token does not match device id'))
    const persistedExisting = photoMetadataRepository ? await photoMetadataRepository.findByIdempotency(u.id, deviceId, idem) : undefined
    if (persistedExisting) return reply.code(200).send(uploadResult(persistedExisting, store))
    const existing = [...store.photos.values()].find(p => p.authorId === u.id && p.idempotencyKey === idem && p.deviceId === deviceId)
    if (existing) return reply.code(200).send(uploadResult(existing, store))
    const usedToday = store.dailyUploadCount(u.id, deviceId)
    if (usedToday >= store.uploadDailyLimit) {
      const tomorrow = new Date(); tomorrow.setUTCHours(24, 0, 0, 0)
      return reply.code(429).send({ ...errorBody('RATE_LIMITED', 'daily upload limit reached'), retry_after: Math.max(1, Math.ceil((tomorrow.getTime() - Date.now()) / 1000)) })
    }
    let caption: string | null = null
    if (r.headers['x-caption']) {
      try { caption = decodeURIComponent(String(r.headers['x-caption'])) } catch { return reply.code(400).send(errorBody('BAD_REQUEST', 'X-Caption must be URL-encoded UTF-8')) }
      if (caption.length > 140) return reply.code(400).send(errorBody('BAD_REQUEST', 'caption must be at most 140 characters'))
    }
    let circle = '小圈'
    if (r.headers['x-circle']) {
      try { circle = decodeURIComponent(String(r.headers['x-circle'])) } catch { return reply.code(400).send(errorBody('BAD_REQUEST', 'X-Circle must be URL-encoded UTF-8')) }
      if (!circle || circle.length > 32) return reply.code(400).send(errorBody('BAD_REQUEST', 'circle must be between 1 and 32 characters'))
    }
    const width = Number(r.headers['x-width'] ?? 320)
    const height = Number(r.headers['x-height'] ?? 240)
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 8192 || height > 8192) return reply.code(400).send(errorBody('BAD_REQUEST', 'width and height must be integers from 1 to 8192'))
    const beauty = Number(r.headers['x-beauty'] ?? 0)
    if (!Number.isFinite(beauty) || beauty < 0 || beauty > 100) return reply.code(400).send(errorBody('BAD_REQUEST', 'beauty must be between 0 and 100'))
    const id = photoMetadataRepository ? randomUUID() : `p_${randomUUID()}`; const p = { id, authorId: u.id, filterId: String(r.headers['x-filter-id'] ?? 'none'), playType: String(r.headers['x-play-type'] ?? 'ccd'), beauty, sticker: String(r.headers['x-sticker'] ?? 'none'), caption, circle, width, height, createdAt: new Date().toISOString(), original: body, processed: body, idempotencyKey: idem, deviceId }
    try { await files.save(p) } catch { return reply.code(503).send(errorBody('STORAGE_UNAVAILABLE', 'photo storage unavailable')) }
    if (photoMetadataRepository) {
      try { await photoMetadataRepository.create(p) } catch { await files.remove(p).catch(() => undefined); return reply.code(503).send(errorBody('PERSISTENCE_UNAVAILABLE', 'photo metadata unavailable')) }
    }
    store.photos.set(id, p)
    return reply.code(201).send(uploadResult(p, store))
  })
  app.get('/photos/:id/image', async (r, reply) => {
    const { account: u, device } = await auth(r, store, devicePairStore)
    const photoId = String((r.params as any).id)
    const p = store.photos.get(photoId) ?? (photoMetadataRepository ? await photoMetadataRepository.findById(photoId) : undefined)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    if (!p) return reply.code(404).send(errorBody('NOT_FOUND', 'photo not found'))
    if (p.draftJobId && p.authorId !== u.id) return reply.code(403).send(errorBody('FORBIDDEN', 'AI draft is private'))
    if (!store.isFriend(u.id, p.authorId)) return reply.code(403).send(errorBody('FORBIDDEN', 'photo is not visible to this user'))
    const requestedSize = (r.query as { size?: string }).size
    if (requestedSize !== undefined && requestedSize !== '320') return reply.code(400).send(errorBody('BAD_REQUEST', 'only size=320 is supported'))
    try {
      const image = await files.read(p)
      const deviceVariant = Boolean(device) || requestedSize === '320'
      // The documented device download remains a baseline 320×240 JPEG; do not reapply a look.
      const output = deviceVariant ? (await renderPhoto(image, { presetId: 'none', intensity: 0, aiGenerated: p.aiGenerated === true })).device : image
      return reply.header('Vary', 'Authorization').header('cache-control', p.draftJobId || deviceVariant ? 'private, no-store' : 'private, max-age=31536000, immutable').type('image/jpeg').send(output)
    } catch {
      return reply.code(503).send(errorBody('STORAGE_UNAVAILABLE', 'photo storage unavailable'))
    }
  })
  const feed = async (r: FastifyRequest, reply: any, mine = false) => {
    const { account: u } = await auth(r, store, devicePairStore)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const rawLimit = (r.query as any)?.limit
    const parsedLimit = rawLimit === undefined ? 8 : Number(rawLimit)
    if (!Number.isInteger(parsedLimit) || parsedLimit < 1) return reply.code(400).send(errorBody('BAD_REQUEST', 'limit must be a positive integer'))
    const limit = Math.min(32, parsedLimit)
    const photos = (mine ? [...store.photos.values()].filter(p => !p.draftJobId && p.authorId === u.id) : store.visiblePhotos(u.id))
    const etag = `W/\"feed-${photos.map(p => p.id).join('-')}\"`
    if (r.headers['if-none-match'] === etag) return reply.code(304).header('ETag', etag).send()
    return reply.header('ETag', etag).send({ items: photos.slice(0, limit).map(p => item(p, u.id, store)), next_cursor: null, etag })
  }
  app.get('/feed', (r, reply) => feed(r, reply, false)); app.get('/photos/mine', (r, reply) => feed(r, reply, true))
  app.delete('/photos/:id', async (r, reply) => {
    const token = String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, '')
    const persistentDevice = devicePairStore ? await devicePairStore.deviceForToken(token) : undefined
    const u = store.userForToken(token) ?? (persistentDevice?.userId ? store.user(persistentDevice.userId) : undefined) ?? persistentDevice?.user
    const device = store.deviceForToken(token) ?? persistentDevice
    const photoId = String((r.params as any).id)
    const p = store.photos.get(photoId) ?? (photoMetadataRepository ? await photoMetadataRepository.findById(photoId) : undefined)
    if (!u) return reply.code(device ? 403 : 401).send(errorBody(device ? 'FORBIDDEN' : 'TOKEN_INVALID', device ? 'device token cannot delete photos' : 'token invalid'))
    if (!p) return reply.code(404).send(errorBody('NOT_FOUND','photo not found'))
    if (p.authorId !== u.id) return reply.code(403).send(errorBody('FORBIDDEN','not owner'))
    try { await files.remove(p) } catch { return reply.code(503).send(errorBody('STORAGE_UNAVAILABLE', 'photo storage unavailable')) }
    if (photoMetadataRepository) {
      try { await photoMetadataRepository.remove(p.id) } catch { return reply.code(503).send(errorBody('PERSISTENCE_UNAVAILABLE', 'photo metadata unavailable')) }
    }
    store.photos.delete(p.id)
    return reply.code(204).send()
  })
}
function uploadResult(p: any, store?: DemoStore) { return { photo_id: p.id, url: `/v1/photos/${p.id}/image`, created_at: p.createdAt, daily_remaining: store ? Math.max(0, store.uploadDailyLimit - store.dailyUploadCount(p.authorId, p.deviceId ?? 'web')) : 60 } }
function item(p: any, uid: string, store: DemoStore) { const author = store.user(p.authorId); const my = Object.keys(store.reactionsFor(p.id)).filter(type => store.reactions.has(`${p.id}:${type}:${uid}`)); return { photo_id: p.id, author: { username: author?.username ?? '', display_name: author?.displayName ?? '' }, filter_id: p.filterId, play_type: p.playType ?? 'ccd', beauty: p.beauty ?? 0, sticker: p.sticker ?? 'none', caption: p.caption, circle: p.circle ?? '小圈', created_at: p.createdAt, width: p.width, height: p.height, image_url: `/v1/photos/${p.id}/image`, reactions: store.reactionsFor(p.id), my_reactions: my, mine: p.authorId === uid } }
