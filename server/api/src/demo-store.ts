import { randomUUID, scryptSync, randomBytes, timingSafeEqual } from 'node:crypto'

export function hashPassword(password: string, salt?: string): { salt: string; hash: string } {
  const useSalt = salt ?? randomBytes(16).toString('hex')
  const hash = scryptSync(password, useSalt, 64).toString('hex')
  return { salt: useSalt, hash }
}

export function verifyPassword(password: string, salt: string, hash: string): boolean {
  const candidate = scryptSync(password, salt, 64)
  const expected = Buffer.from(hash, 'hex')
  return candidate.length === expected.length && timingSafeEqual(candidate, expected)
}

// A valid 1x1 JPEG keeps the seeded demo feed renderable without external assets.
const DEMO_JPEG = Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/AYf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AYf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Aqf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/IV//2gAMAwEAAgADAAAAEP/EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIBAT8QH//EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEAAT8QH//Z', 'base64')

export type User = { id: string; username: string; displayName: string; friendCode: string; passwordSalt?: string; passwordHash?: string }
export type Session = { token: string; userId: string; expiresAt: number }
export type Photo = { id: string; authorId: string; filterId: string; playType?: string; beauty?: number; sticker?: string; caption: string | null; circle?: string; circleId?: string; width: number; height: number; createdAt: string; original: Buffer; processed: Buffer; idempotencyKey?: string; deviceId?: string; draftJobId?: string; aiGenerated?: boolean; generation?: { provider: string; model: string; promptVersion: string; requestId?: string; referenceWarnings?: string[]; presetVersion?: string; intensity?: number; seed?: number } }
export type Message = { id: string; from: string; to: string; text?: string; photoId?: string; createdAt: string }
export type Job = { id: string; ownerId: string; materialIds: string[]; provider?: string; status: 'queued'|'processing'|'completed'|'failed'; resultPhotoId?: string; publishedPhotoId?: string; error?: string; createdAt: string }
export type FriendRequest = { id: string; requesterId: string; addresseeId: string; status: 'pending' | 'accepted'; createdAt: string }
export type Circle = { id: string; name: string; type: 'small' | 'big'; ownerId?: string; curatedPhotoIds: string[] }
export type DeviceConfig = { id: string; filter_id: string; play_type: string; beauty: number; sticker: string; updated_at: string }

export class DemoStore {
  readonly provider = 'demo' as const
  readonly uploadDailyLimit: number
  users = new Map<string, User>()
  tokens = new Map<string, string>()
  sessions = new Map<string, Session>()
  photos = new Map<string, Photo>()
  messages: Message[] = []
  reactions = new Map<string, Set<string>>()
  jobs = new Map<string, Job>()
  friendships = new Set<string>()
  friendRequests = new Map<string, FriendRequest>()
  circles = new Map<string, Circle>()
  circleSubscriptions = new Map<string, Set<string>>()
  devices = new Map<string, { userId?: string; token?: string; pairCode?: string; expiresAt?: number; lastSeen?: string; pendingConfig?: DeviceConfig; activeConfig?: DeviceConfig; configIdempotencyKey?: string; configResponse?: { config_id: string; status: 'queued'; device_id: string; config: DeviceConfig } }>()

  constructor(options: { uploadDailyLimit?: number; seedAgeMs?: number } = {}) {
    this.uploadDailyLimit = Math.max(1, Math.floor(options.uploadDailyLimit ?? Number(process.env.UPLOAD_DAILY_LIMIT ?? 60)))
    const u1 = { id: 'u_demo_1', username: 'ayan', displayName: '阿岩', friendCode: '100001' }
    const u2 = { id: 'u_demo_2', username: 'momo', displayName: '墨墨', friendCode: '100002' }
    const u3 = { id: 'u_demo_3', username: 'luna', displayName: '露娜', friendCode: '100003' }
    ;[u1, u2, u3].forEach(u => this.users.set(u.id, u))
    for (const u of [u1, u2, u3]) {
      const { salt, hash } = hashPassword('demo1234')
      this.users.set(u.id, { ...u, passwordSalt: salt, passwordHash: hash })
    }
    this.tokens.set('demo-token', u1.id); this.tokens.set('demo-user-2', u2.id); this.tokens.set('demo-user-3', u3.id); this.tokens.set('device-token-1', u1.id)
    this.friendships.add(`${u1.id}:${u2.id}`); this.friendships.add(`${u2.id}:${u1.id}`)
    this.friendships.add(`${u1.id}:${u3.id}`); this.friendships.add(`${u3.id}:${u1.id}`)
    const now = Date.now() - (options.seedAgeMs ?? 0)
    const seeds = [
      { authorId: u2.id, filterId: 'warm', playType: 'ccd', caption: '傍晚的风从窗台进来。', circle: '傍晚的天空' },
      { authorId: u1.id, filterId: 'none', playType: 'beauty', caption: '把今天折成一张小卡。', circle: '小圈' },
      { authorId: u3.id, filterId: 'film', playType: 'ccd', caption: '今天也有好好在场。', circle: '宿舍窗台' },
      { authorId: u1.id, filterId: 'vivid', playType: 'ccd', caption: '留一点颗粒给下一次见面。', circle: '小圈' },
    ] as const
    for (const [index, seed] of seeds.entries()) {
      const id = `p_demo_${index + 1}`; const jpeg = DEMO_JPEG
      this.photos.set(id, { id, ...seed, beauty: seed.playType === 'beauty' ? 42 : 0, sticker: 'none', width: 320, height: 240, createdAt: new Date(now - (index + 1) * 60000).toISOString(), original: jpeg, processed: jpeg })
    }
    this.seedCircles(u1.id, u2.id, u3.id, options.seedAgeMs ?? 0)
  }
  private seedCircles(u1: string, u2: string, u3: string, seedAgeMs = 0) {
    const sky: Circle = { id: 'c_sky', name: '傍晚的天空', type: 'big', curatedPhotoIds: [] }
    const film: Circle = { id: 'c_film', name: '胶片味', type: 'big', curatedPhotoIds: [] }
    const desk: Circle = { id: 'c_desk', name: '宿舍窗台', type: 'big', curatedPhotoIds: [] }
    ;[sky, film, desk].forEach(c => this.circles.set(c.id, c))
    // Curator accounts publish big-circle picks; they are nobody's friend so
    // curated photos stay gated behind subscription.
    const curators = [
      { id: 'u_cur_1', username: 'curator_sky', displayName: '天空收录员', friendCode: '900001' },
      { id: 'u_cur_2', username: 'curator_wind', displayName: '风的切片', friendCode: '900002' },
      { id: 'u_cur_3', username: 'curator_grain', displayName: '颗粒研究所', friendCode: '900003' },
    ]
    const [u_cur_1, u_cur_2, u_cur_3] = curators.map(c => { this.users.set(c.id, c); return c.id })
    // Extra curated big-circle photos (single seed JPEG keeps payloads small).
    const curated: Array<{ id: string; authorId: string; caption: string; circleId: string; filterId: string }> = [
      { id: 'p_cur_1', authorId: u_cur_1, caption: '云被夕阳烫了个边。', circleId: sky.id, filterId: 'warm' },
      { id: 'p_cur_2', authorId: u_cur_2, caption: '风把云吹散了。', circleId: sky.id, filterId: 'film' },
      { id: 'p_cur_5', authorId: u_cur_1, caption: '天边最后一格电。', circleId: sky.id, filterId: 'vivid' },
      { id: 'p_cur_3', authorId: u_cur_3, caption: '颗粒感刚刚好。', circleId: film.id, filterId: 'bw' },
      { id: 'p_cur_4', authorId: u_cur_1, caption: '窗台上的光。', circleId: desk.id, filterId: 'vivid' },
    ]
    const now = Date.now() - seedAgeMs
    for (const [index, c] of curated.entries()) {
      this.photos.set(c.id, { id: c.id, authorId: c.authorId, filterId: c.filterId, playType: 'ccd', beauty: 0, sticker: 'none', caption: c.caption, circle: this.circles.get(c.circleId)?.name, circleId: c.circleId, width: 320, height: 240, createdAt: new Date(now - (index + 10) * 60000).toISOString(), original: DEMO_JPEG, processed: DEMO_JPEG })
      this.circles.get(c.circleId)?.curatedPhotoIds.push(c.id)
    }
    // ayan already subscribes to 傍晚的天空 so device mixing has data.
    this.circleSubscriptions.set(u1, new Set([sky.id, film.id]))
  }
  userForToken(token?: string) {
    if (!token) return undefined
    const session = this.sessions.get(token)
    if (session) {
      if (session.expiresAt <= Date.now()) { this.sessions.delete(token); return undefined }
      return this.users.get(session.userId)
    }
    return this.users.get(this.tokens.get(token) ?? '')
  }
  createSession(userId: string, ttlMs = 72 * 3600 * 1000): Session {
    const token = `sess_${randomUUID().replace(/-/g, '')}`
    const session: Session = { token, userId, expiresAt: Date.now() + ttlMs }
    this.sessions.set(token, session)
    return session
  }
  revokeSession(token: string) { this.sessions.delete(token) }
  deviceForToken(token?: string) { if (!token) return undefined; for (const [id, device] of this.devices) if (device.token === token) return { id, ...device }; return undefined }
  user(id: string) { return this.users.get(id) }
  dailyUploadCount(authorId: string, deviceId: string, now = Date.now()) {
    const day = new Date(now).toISOString().slice(0, 10)
    return [...this.photos.values()].filter(photo => !photo.draftJobId && photo.authorId === authorId && photo.deviceId === deviceId && photo.createdAt.slice(0, 10) === day).length
  }
  isFriend(a: string, b: string) { return a === b || this.friendships.has(`${a}:${b}`) }
  subscriptionsFor(userId: string) { return this.circleSubscriptions.get(userId) ?? new Set<string>() }
  subscribedBigCircles(userId: string) { return [...this.subscriptionsFor(userId)].map(id => this.circles.get(id)).filter((c): c is Circle => Boolean(c && c.type === 'big')) }
  curatedPhotosFor(userId: string) {
    const ids = this.subscribedBigCircles(userId).flatMap(c => c.curatedPhotoIds)
    return ids.map(id => this.photos.get(id)).filter((p): p is Photo => Boolean(p && !p.draftJobId))
  }
  canSeePhoto(userId: string, photo: Photo) {
    if (photo.authorId === userId) return true
    if (photo.circleId) {
      // Big-circle content is gated by subscription, not by friendship.
      const circle = this.circles.get(photo.circleId)
      return circle?.type === 'big' && this.subscriptionsFor(userId).has(photo.circleId)
    }
    return this.isFriend(userId, photo.authorId)
  }
  visiblePhotos(userId: string) { return [...this.photos.values()].filter(p => !p.draftJobId && this.canSeePhoto(userId, p)).sort((a,b) => b.createdAt.localeCompare(a.createdAt)) }
  deviceFeedPhotos(userId: string) {
    const friendPhotos = [...this.photos.values()].filter(p => !p.draftJobId && this.isFriend(userId, p.authorId)).sort((a,b) => b.createdAt.localeCompare(a.createdAt))
    const cutoff = Date.now() - 24 * 3600 * 1000
    const freshFriend = friendPhotos.some(p => new Date(p.createdAt).getTime() >= cutoff)
    if (freshFriend) return friendPhotos
    const curated = this.curatedPhotosFor(userId).filter(p => p.authorId !== userId && !this.isFriend(userId, p.authorId)).slice(0, 10)
    return [...friendPhotos, ...curated].sort((a,b) => b.createdAt.localeCompare(a.createdAt))
  }
  reactionsFor(photoId: string) {
    const out: Record<string, number> = { heart: 0, thumbsup: 0, wow: 0 }
    for (const key of this.reactions.keys()) if (key.startsWith(`${photoId}:`)) out[key.split(':')[1]]++
    return out
  }
  toggleReaction(photoId: string, userId: string, type: string) { const key = `${photoId}:${type}:${userId}`; if (this.reactions.has(key)) this.reactions.delete(key); else this.reactions.set(key, new Set([userId])); }
}

export const errorBody = (code: string, message: string) => ({ error: { code, message } })
