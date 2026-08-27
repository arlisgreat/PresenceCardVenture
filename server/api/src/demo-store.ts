import { randomUUID } from 'node:crypto'

// A valid 1x1 JPEG keeps the seeded demo feed renderable without external assets.
const DEMO_JPEG = Buffer.from('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/AYf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AYf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Aqf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/IV//2gAMAwEAAgADAAAAEP/EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIBAT8QH//EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEAAT8QH//Z', 'base64')

export type User = { id: string; username: string; displayName: string; friendCode: string }
export type Photo = { id: string; authorId: string; filterId: string; playType?: string; beauty?: number; sticker?: string; caption: string | null; circle?: string; width: number; height: number; createdAt: string; original: Buffer; processed: Buffer; idempotencyKey?: string; deviceId?: string }
export type Message = { id: string; from: string; to: string; text?: string; photoId?: string; createdAt: string }
export type Job = { id: string; ownerId: string; materialIds: string[]; provider?: string; status: 'queued'|'processing'|'completed'|'failed'; resultPhotoId?: string; error?: string; createdAt: string }
export type FriendRequest = { id: string; requesterId: string; addresseeId: string; status: 'pending' | 'accepted'; createdAt: string }
export type DeviceConfig = { id: string; filter_id: string; play_type: string; beauty: number; sticker: string; updated_at: string }

export class DemoStore {
  readonly uploadDailyLimit: number
  users = new Map<string, User>()
  tokens = new Map<string, string>()
  photos = new Map<string, Photo>()
  messages: Message[] = []
  reactions = new Map<string, Set<string>>()
  jobs = new Map<string, Job>()
  friendships = new Set<string>()
  friendRequests = new Map<string, FriendRequest>()
  devices = new Map<string, { userId?: string; token?: string; pairCode?: string; expiresAt?: number; lastSeen?: string; pendingConfig?: DeviceConfig; configIdempotencyKey?: string; configResponse?: { config_id: string; status: 'queued'; device_id: string; config: DeviceConfig } }>()

  constructor(options: { uploadDailyLimit?: number } = {}) {
    this.uploadDailyLimit = Math.max(1, Math.floor(options.uploadDailyLimit ?? Number(process.env.UPLOAD_DAILY_LIMIT ?? 60)))
    const u1 = { id: 'u_demo_1', username: 'ayan', displayName: '阿岩', friendCode: '100001' }
    const u2 = { id: 'u_demo_2', username: 'momo', displayName: '墨墨', friendCode: '100002' }
    const u3 = { id: 'u_demo_3', username: 'luna', displayName: '露娜', friendCode: '100003' }
    ;[u1, u2, u3].forEach(u => this.users.set(u.id, u))
    this.tokens.set('demo-token', u1.id); this.tokens.set('demo-user-2', u2.id); this.tokens.set('demo-user-3', u3.id); this.tokens.set('device-token-1', u1.id)
    this.friendships.add(`${u1.id}:${u2.id}`); this.friendships.add(`${u2.id}:${u1.id}`)
    this.friendships.add(`${u1.id}:${u3.id}`); this.friendships.add(`${u3.id}:${u1.id}`)
    const now = Date.now()
    for (const [i, authorId] of [[1, u2.id], [2, u1.id], [3, u3.id], [4, u1.id]] as const) {
      const id = `p_demo_${i}`; const jpeg = DEMO_JPEG
      this.photos.set(id, { id, authorId, filterId: i === 1 ? 'warm' : 'none', caption: `Demo photo ${i}`, circle: i === 1 ? '傍晚的天空' : '小圈', width: 320, height: 240, createdAt: new Date(now - i * 60000).toISOString(), original: jpeg, processed: jpeg })
    }
  }
  userForToken(token?: string) { return token ? this.users.get(this.tokens.get(token) ?? '') : undefined }
  deviceForToken(token?: string) { if (!token) return undefined; for (const [id, device] of this.devices) if (device.token === token) return { id, ...device }; return undefined }
  user(id: string) { return this.users.get(id) }
  dailyUploadCount(authorId: string, deviceId: string, now = Date.now()) {
    const day = new Date(now).toISOString().slice(0, 10)
    return [...this.photos.values()].filter(photo => photo.authorId === authorId && photo.deviceId === deviceId && photo.createdAt.slice(0, 10) === day).length
  }
  isFriend(a: string, b: string) { return a === b || this.friendships.has(`${a}:${b}`) }
  visiblePhotos(userId: string) { return [...this.photos.values()].filter(p => this.isFriend(userId, p.authorId)).sort((a,b) => b.createdAt.localeCompare(a.createdAt)) }
  reactionsFor(photoId: string) {
    const out: Record<string, number> = { heart: 0, thumbsup: 0, wow: 0 }
    for (const key of this.reactions.keys()) if (key.startsWith(`${photoId}:`)) out[key.split(':')[1]]++
    return out
  }
  toggleReaction(photoId: string, userId: string, type: string) { const key = `${photoId}:${type}:${userId}`; if (this.reactions.has(key)) this.reactions.delete(key); else this.reactions.set(key, new Set([userId])); }
}

export const errorBody = (code: string, message: string) => ({ error: { code, message } })
