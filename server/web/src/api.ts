import { fetchWithUserSession, setUserToken, clearUserToken } from './user-session.js'

export type PlayType = 'beauty' | 'ccd' | 'template'

export type FeedItem = {
  id: string
  photo_id?: string
  author: { username: string; display_name: string }
  filter_id: string
  play_type?: PlayType
  beauty?: number
  sticker?: string
  caption?: string | null
  created_at: string
  image_url: string
  reactions: Record<string, number>
  my_reactions?: string[]
  mine?: boolean
  circle?: string
  circle_id?: string | null
}

export type Message = {
  id: string
  sender: string
  senderName: string
  body: string
  createdAt: string
  kind?: 'text' | 'image'
  photo_id?: string
  image_url?: string
}

export type Friend = { username: string; display_name: string }
export type FriendRequest = { id: string; status: 'pending' | 'accepted'; direction: 'incoming' | 'outgoing'; requester: Friend; addressee: Friend; created_at: string }
export type CurrentUser = { id: string; username: string; display_name: string; friend_code: string }

export type CircleInfo = { id: string; name: string; type: 'small' | 'big'; joined: boolean; photo_count: number; subscriber_count: number }

export type AuthResult = { token: string; expires_in: number; user: CurrentUser }

export type AiJob = {
  id: string
  status: 'queued' | 'processing' | 'completed' | 'failed'
  resultUrl?: string
  message?: string
  publishedPhotoId?: string
}

export type PairStatus = {
  status: 'pending' | 'bound'
  device_token?: string
  user?: { username?: string; display_name?: string }
}

class ApiError extends Error {
  constructor(public readonly status: number, message: string) {
    super(message)
    this.name = 'ApiError'
  }
}

const API = '/v1'

type ProcessedImage = { blob: Blob | File; width: number; height: number }

function clamp(value: number) {
  return Math.max(0, Math.min(255, Math.round(value)))
}

function offlineFallback<T>(error: unknown, fallback: () => T): T {
  if (error instanceof TypeError) return fallback()
  throw error
}

async function jpegPayload(file: File, options: { filterId: string; beauty: number; sticker: string }): Promise<ProcessedImage> {
  if (!['image/jpeg', 'image/png'].includes(file.type)) throw new Error('仅支持 JPG 或 PNG 图片')
  if (typeof createImageBitmap !== 'function') return { blob: file, width: 0, height: 0 }
  const bitmap = await createImageBitmap(file)
  const canvas = document.createElement('canvas')
  const scale = Math.min(1, 1600 / Math.max(bitmap.width, bitmap.height))
  const width = Math.max(1, Math.round(bitmap.width * scale))
  const height = Math.max(1, Math.round(bitmap.height * scale))
  canvas.width = width
  canvas.height = height
  const context = canvas.getContext('2d', { willReadFrequently: true })
  if (!context) { bitmap.close(); return { blob: file, width, height } }
  context.drawImage(bitmap, 0, 0, width, height)
  bitmap.close()
  const pixels = context.getImageData(0, 0, width, height)
  const intensity = Math.max(0, Math.min(1, options.beauty / 100))
  for (let index = 0; index < pixels.data.length; index += 4) {
    let red = pixels.data[index]
    let green = pixels.data[index + 1]
    let blue = pixels.data[index + 2]
    if (options.filterId === 'warm') { red += 14; green += 7; blue -= 3 }
    if (options.filterId === 'film') { red = red * .92 + 10; green = green * .98 + 4; blue = blue * .86 + 8 }
    if (options.filterId === 'vivid') { red = (red - 128) * 1.12 + 128; green = (green - 128) * 1.12 + 128; blue = (blue - 128) * 1.12 + 128 }
    if (options.filterId === 'bw') { const gray = red * .299 + green * .587 + blue * .114; red = gray; green = gray; blue = gray }
    if (intensity > 0) { red += 18 * intensity; green += 18 * intensity; blue += 18 * intensity }
    pixels.data[index] = clamp(red)
    pixels.data[index + 1] = clamp(green)
    pixels.data[index + 2] = clamp(blue)
  }
  context.putImageData(pixels, 0, 0)
  if (options.sticker === 'star') {
    context.fillStyle = 'rgba(255, 247, 210, .92)'
    context.font = `bold ${Math.max(18, Math.round(width / 16))}px sans-serif`
    context.fillText('✦', Math.max(10, width - 42), Math.max(28, height - 18))
  } else if (options.sticker === 'date') {
    context.fillStyle = 'rgba(255, 255, 255, .86)'
    context.font = `600 ${Math.max(10, Math.round(width / 70))}px monospace`
    const now = new Date()
    const localDate = [now.getFullYear(), String(now.getMonth() + 1).padStart(2, '0'), String(now.getDate()).padStart(2, '0')].join('·')
    context.fillText(localDate, Math.max(10, width - 110), Math.max(20, height - 18))
  }
  const blob = await new Promise<Blob | null>(resolve => canvas.toBlob(resolve, 'image/jpeg', .88))
  if (!blob) throw new Error('图片转换失败')
  return { blob, width, height }
}

const demoFeed: FeedItem[] = [
  { id: 'seed-1', author: { username: 'momo', display_name: '墨墨' }, filter_id: 'warm', caption: '傍晚的风从窗台进来。', created_at: new Date(Date.now() - 1000 * 60 * 18).toISOString(), image_url: '/assets/feed-window.jpg', reactions: { heart: 12, star: 4 }, my_reactions: [], circle: '傍晚的天空' },
  { id: 'seed-2', author: { username: 'ayan', display_name: '阿岩' }, filter_id: 'film', caption: '把今天折成一张小卡。', created_at: new Date(Date.now() - 1000 * 60 * 72).toISOString(), image_url: '/assets/feed-portrait.jpg', reactions: { heart: 8, star: 2 }, my_reactions: [], circle: '小圈' },
  { id: 'seed-3', author: { username: 'luna', display_name: '露娜' }, filter_id: 'ccd', caption: '今天也有好好在场。', created_at: new Date(Date.now() - 1000 * 60 * 160).toISOString(), image_url: '/assets/feed-friends.jpg', reactions: { heart: 19, star: 7 }, my_reactions: [], circle: '宿舍窗台' },
]

const demoAssetByPhotoId: Record<string, string> = {
  p_demo_1: '/assets/feed-window.jpg',
  p_demo_2: '/assets/feed-portrait.jpg',
  p_demo_3: '/assets/feed-friends.jpg',
  p_demo_4: '/assets/feed-window.jpg',
}

export function filterFeedByCircle(feed: FeedItem[], circle: string): FeedItem[] {
  if (circle === '全部') return feed
  return feed.filter(item => item.circle === circle)
}

async function requestWithToken<T>(path: string, token: string, init?: RequestInit): Promise<T> {
  const headers: Record<string, string> = { Authorization: `Bearer ${token}`, ...(init?.headers as Record<string, string> ?? {}) }
  if (init?.body) headers['Content-Type'] = 'application/json'
  const response = await fetch(`${API}${path}`, { ...init, headers })
  if (!response.ok) throw new Error((await response.text()) || `Request failed: ${response.status}`)
  if (response.status === 204) return undefined as T
  return response.json() as Promise<T>
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const headers = new Headers(init?.headers)
  if (init?.body) headers.set('Content-Type', 'application/json')
  const response = await fetchWithUserSession(`${API}${path}`, { ...init, headers })
  if (!response.ok) throw new Error((await response.text()) || `Request failed: ${response.status}`)
  if (response.status === 204) return undefined as T
  return response.json() as Promise<T>
}

export async function getCurrentUser(): Promise<CurrentUser> {
  return request<CurrentUser>('/me')
}

export async function getFeed(circleId?: string): Promise<FeedItem[]> {
  try {
    const query = circleId && circleId !== 'all' ? `&circle_id=${encodeURIComponent(circleId)}` : ''
    const data = await request<{ items?: FeedItem[] }>(`/feed?limit=20${query}`, { cache: 'no-store' })
    return mapFeedItems(data.items ?? [])
  } catch (error) {
    const circleName = circleId ? circlesByIdFallback[circleId] : undefined
    return offlineFallback(error, () => circleName ? demoFeed.filter(item => item.circle === circleName) : demoFeed)
  }
}

export type FeedPage = { items: FeedItem[]; nextCursor: string | null }

export async function getFeedPage(circleId?: string, cursor?: string): Promise<FeedPage> {
  const circleQuery = circleId && circleId !== 'all' ? `&circle_id=${encodeURIComponent(circleId)}` : ''
  const cursorQuery = cursor ? `&cursor=${encodeURIComponent(cursor)}` : ''
  try {
    const data = await request<{ items?: FeedItem[]; next_cursor?: string | null }>(`/feed?limit=8${circleQuery}${cursorQuery}`, { cache: 'no-store' })
    return { items: mapFeedItems(data.items ?? []), nextCursor: data.next_cursor ?? null }
  } catch (error) {
    if (cursor) return { items: [], nextCursor: null }
    const circleName = circleId ? circlesByIdFallback[circleId] : undefined
    const items = offlineFallback(error, () => circleName ? demoFeed.filter(item => item.circle === circleName) : demoFeed)
    return { items, nextCursor: null }
  }
}

function mapFeedItems(items: FeedItem[]): FeedItem[] {
  return items.map(item => item.photo_id?.startsWith('p_demo_') ? { ...item, id: item.photo_id, image_url: demoAssetByPhotoId[item.photo_id] ?? item.image_url } : { ...item, id: item.photo_id ?? item.id })
}

const circlesByIdFallback: Record<string, string> = { c_small: '小圈', c_sky: '傍晚的天空', c_film: '胶片味', c_desk: '宿舍窗台' }

export async function getCircles(): Promise<CircleInfo[]> {
  const data = await request<{ items: CircleInfo[] }>('/circles')
  return data.items
}

export async function createCircle(name: string): Promise<CircleInfo> {
  const circle = await request<CircleInfo>('/circles', { method: 'POST', body: JSON.stringify({ name }) })
  return { ...circle, joined: circle.joined ?? true, subscriber_count: circle.subscriber_count ?? 1, photo_count: circle.photo_count ?? 0 }
}

export async function joinCircle(id: string): Promise<CircleInfo> {
  const circle = await request<CircleInfo>(`/circles/${id}/join`, { method: 'POST', body: JSON.stringify({}) })
  return { ...circle, joined: true }
}

export async function leaveCircle(id: string): Promise<void> {
  await request(`/circles/${id}/leave`, { method: 'POST', body: JSON.stringify({}) })
}

export async function getCircleFeed(circleId: string, limit = 32): Promise<FeedItem[]> {
  const data = await request<{ items: FeedItem[] }>(`/circles/${circleId}/feed?limit=${limit}`, { cache: 'no-store' })
  return (data.items ?? []).map(item => ({ ...item, id: item.photo_id ?? item.id }))
}

export async function registerAccount(options: { username: string; password: string; displayName?: string; inviteCode?: string }): Promise<AuthResult> {
  const result = await request<AuthResult & { user: CurrentUser }>('/auth/register', {
    method: 'POST',
    body: JSON.stringify({ username: options.username, password: options.password, display_name: options.displayName, invite_code: options.inviteCode }),
  })
  setUserToken(result.token)
  return result
}

export async function loginAccount(username: string, password: string): Promise<AuthResult> {
  const result = await request<AuthResult & { user: CurrentUser }>('/auth/login', { method: 'POST', body: JSON.stringify({ username, password }) })
  setUserToken(result.token)
  return result
}

export async function logoutAccount(): Promise<void> {
  try { await request('/auth/logout', { method: 'POST', body: JSON.stringify({}) }) } catch { /* token may already be invalid */ }
  clearUserToken()
}

export async function uploadPhoto(file: File, options: { filterId: string; caption: string; play: PlayType; beauty: number; sticker: string; circle?: string; circleId?: string }): Promise<FeedItem> {
  const localUrl = URL.createObjectURL(file)
  try {
    const processed = await jpegPayload(file, options)
    const payload = processed.blob
    const circle = options.circle ?? '小圈'
    const response = await fetchWithUserSession(`${API}/photos`, { method: 'POST', body: payload, headers: { 'Content-Type': 'image/jpeg', 'Idempotency-Key': `web-${Date.now()}`, 'X-Filter-Id': options.filterId, 'X-Play-Type': options.play, 'X-Beauty': String(options.beauty), 'X-Sticker': options.sticker, 'X-Circle': encodeURIComponent(circle), ...(options.circleId ? { 'X-Circle-Id': options.circleId } : {}), 'X-Caption': encodeURIComponent(options.caption), 'X-Width': String(processed.width || 1080), 'X-Height': String(processed.height || 1350) } })
    if (!response.ok) throw new ApiError(response.status, (await response.text()) || `Request failed: ${response.status}`)
    const result = await response.json() as { photo_id: string; url?: string; created_at?: string }
    const imageUrl = result.url ?? localUrl
    if (result.url) URL.revokeObjectURL(localUrl)
    return { id: result.photo_id, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, play_type: options.play, beauty: options.beauty, sticker: options.sticker, caption: options.caption, created_at: result.created_at ?? new Date().toISOString(), image_url: imageUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle, circle_id: options.circleId ?? null }
  } catch (error) {
    if (!(error instanceof TypeError)) throw error
    return { id: `local-${Date.now()}`, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, play_type: options.play, beauty: options.beauty, sticker: options.sticker, caption: options.caption || '刚刚释放了一张照片。', created_at: new Date().toISOString(), image_url: localUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle: options.circle ?? '小圈', circle_id: options.circleId ?? null }
  }
}

export type DevicePhotoUploadResult = { photo_id: string; url?: string; created_at?: string; daily_remaining?: number }

export async function uploadDevicePhoto(payload: Blob, deviceToken: string, deviceId: string, idempotencyKey: string): Promise<DevicePhotoUploadResult> {
  const response = await fetch(`${API}/photos`, {
    method: 'POST',
    body: payload,
    headers: {
      Authorization: `Bearer ${deviceToken}`,
      'Content-Type': 'image/jpeg',
      'Idempotency-Key': idempotencyKey,
      'X-Device-ID': deviceId,
      'X-Filter-Id': 'none',
      'X-Play-Type': 'beauty',
      'X-Beauty': '0',
      'X-Sticker': 'none',
      'X-Width': '320',
      'X-Height': '240',
    },
  })
  if (!response.ok) throw new Error((await response.text()) || `Request failed: ${response.status}`)
  return response.json() as Promise<DevicePhotoUploadResult>
}

export async function reactToPhoto(id: string, active: boolean): Promise<void> {
  if (active) await request(`/photos/${id}/reactions`, { method: 'POST', body: JSON.stringify({ type: 'heart' }) })
  else await request(`/photos/${id}/reactions/heart`, { method: 'DELETE' })
}

export async function pokePhoto(id: string): Promise<void> {
  await request(`/photos/${id}/reactions`, { method: 'POST', body: JSON.stringify({ type: 'wow' }) })
}

export async function deletePhoto(id: string): Promise<void> {
  if (id.startsWith('local-') || id.startsWith('seed-')) return
  await request(`/photos/${id}`, { method: 'DELETE' })
}

export async function getMessages(friend = 'luna'): Promise<Message[]> {
  try { return await request<Message[]>(`/messages?friend=${encodeURIComponent(friend)}`) } catch (error) {
    return offlineFallback(error, () => [
      { id: 'm1', sender: 'luna', senderName: '露娜', body: '今天的光线好漂亮，像一张旧相纸。', createdAt: new Date(Date.now() - 1000 * 60 * 42).toISOString() },
      { id: 'm2', sender: 'me', senderName: '我', body: '我也拍下来了，晚点放进圈子。', createdAt: new Date(Date.now() - 1000 * 60 * 37).toISOString() },
    ])
  }
}

export async function getFriends(): Promise<Friend[]> {
  try { return await request<Friend[]>('/friends') } catch (error) { return offlineFallback(error, () => [{ username: 'luna', display_name: '露娜' }, { username: 'momo', display_name: '墨墨' }]) }
}

export async function getFriendRequests(): Promise<FriendRequest[]> {
  try { return await request<FriendRequest[]>('/friend-requests') } catch (error) { return offlineFallback(error, () => []) }
}

export async function sendFriendRequest(friendCode: string): Promise<FriendRequest> {
  return request<FriendRequest>('/friend-requests', { method: 'POST', body: JSON.stringify({ friend_code: friendCode }) })
}

export async function acceptFriendRequest(id: string): Promise<FriendRequest> {
  return request<FriendRequest>(`/friend-requests/${id}/accept`, { method: 'POST', body: JSON.stringify({}) })
}

export async function sendMessage(friend: string, body: string, photoId?: string): Promise<Message> {
  try { return await request<Message>('/messages', { method: 'POST', body: JSON.stringify({ friend, body, photo_id: photoId }) }) } catch (error) {
    if (!(error instanceof TypeError)) throw error
    return { id: `m-${Date.now()}`, sender: 'me', senderName: '我', body, createdAt: new Date().toISOString(), kind: photoId ? 'image' : 'text', photo_id: photoId, image_url: photoId && !photoId.startsWith('local-') ? `/v1/photos/${photoId}/image` : undefined }
  }
}

export async function createAiJob(photoIds: string[]): Promise<AiJob> {
  try {
    const response = await request<AiJob & { job_id?: string }>('/ai/jobs', { method: 'POST', body: JSON.stringify({ photo_ids: photoIds, consent: true }) })
    return { ...response, id: response.id ?? response.job_id ?? `ai-${Date.now()}` }
  } catch (error) {
    if (!(error instanceof TypeError)) throw error
    const id = `ai-${Date.now()}`
    return { id, status: 'queued', message: '素材已授权，正在合成一张只属于你们的合照。' }
  }
}

export async function getAiJob(id: string): Promise<AiJob> {
  try {
    const response = await request<AiJob & { result_photo_id?: string; error?: string }>(`/ai/jobs/${id}`)
    return { id, status: response.status, resultUrl: response.resultUrl ?? (response as AiJob & { result_url?: string }).result_url ?? (response.status === 'completed' ? demoFeed[2].image_url : undefined), message: response.message ?? response.error, publishedPhotoId: response.publishedPhotoId }
  } catch (error) {
    if (!(error instanceof TypeError)) throw error
    return { id, status: 'completed', resultUrl: demoFeed[2].image_url, message: '合照已经生成。' }
  }
}

export type PublishedAiResult = { photo_id: string; url: string; created_at: string; caption: string; circle: string; source_job_id: string }

export async function publishAiJob(id: string, options: { caption?: string; circle?: string } = {}): Promise<PublishedAiResult> {
  return request<PublishedAiResult>(`/ai/jobs/${id}/publish`, { method: 'POST', body: JSON.stringify(options) })
}

export async function deleteAiJob(id: string): Promise<void> {
  if (id.startsWith('ai-')) return
  await request(`/ai/jobs/${id}/result`, { method: 'DELETE' })
}

export type DeviceState = { unseen_count: number; pending_friend_requests: number; server_time: string; pending_config?: DeviceConfig | null; active_config?: DeviceConfig | null }

export async function getDeviceState(): Promise<DeviceState> {
  try { return await request<DeviceState>('/device/state') } catch (error) { return offlineFallback(error, () => ({ unseen_count: 3, pending_friend_requests: 1, server_time: new Date().toISOString(), pending_config: null, active_config: null })) }
}

export async function getDeviceStateForToken(deviceToken: string): Promise<DeviceState> {
  return requestWithToken<DeviceState>('/device/state', deviceToken, { method: 'GET' })
}

export type DeviceFeedPage = { items: FeedItem[]; next_cursor?: string | null; etag?: string; not_modified?: boolean }
export type DeviceConfig = { id: string; filter_id: string; play_type: PlayType; beauty: number; sticker: string; updated_at: string }

export async function getDeviceFeed(deviceToken: string, limit = 8, etag?: string): Promise<DeviceFeedPage> {
  const boundedLimit = Math.max(1, Math.min(32, Math.floor(limit)))
  const headers: Record<string, string> = { Authorization: `Bearer ${deviceToken}` }
  if (etag) headers['If-None-Match'] = etag
  const response = await fetch(`${API}/feed?limit=${boundedLimit}`, { method: 'GET', headers })
  if (response.status === 304) return { items: [], next_cursor: null, etag: response.headers.get('ETag') ?? etag, not_modified: true }
  if (!response.ok) throw new Error((await response.text()) || `Request failed: ${response.status}`)
  const data = await response.json() as DeviceFeedPage
  return { ...data, etag: response.headers.get('ETag') ?? data.etag, items: (data.items ?? []).map(item => ({ ...item, id: item.id ?? item.photo_id ?? `device-${item.created_at}` })) }
}

export async function pushDeviceConfig(deviceId: string, config: { filterId: string; playType: PlayType; beauty: number; sticker: string }, idempotencyKey = `web-config-${deviceId}-${Date.now()}`): Promise<{ config_id: string; status: 'queued'; device_id: string; config: DeviceConfig }> {
  return request('/device/config', { method: 'POST', headers: { 'Idempotency-Key': idempotencyKey }, body: JSON.stringify({ device_id: deviceId, filter_id: config.filterId, play_type: config.playType, beauty: config.beauty, sticker: config.sticker }) })
}

export async function requestPairCode(deviceId: string): Promise<{ pair_code: string; expires_in: number }> {
  return request('/pair/code', { method: 'POST', body: JSON.stringify({ device_id: deviceId, fw_version: '0.1.0', hw: 'cams3-lite' }) })
}

export async function bindDevice(deviceId: string, pairCode: string): Promise<void> {
  await request('/pair/bind', { method: 'POST', body: JSON.stringify({ device_id: deviceId, pair_code: pairCode }) })
}

export async function getPairStatus(deviceId: string, pairCode: string): Promise<PairStatus> {
  return request<PairStatus>(`/pair/status?device_id=${encodeURIComponent(deviceId)}&pair_code=${encodeURIComponent(pairCode)}`, { cache: 'no-store' })
}

export async function deviceHeartbeat(deviceToken: string): Promise<void> {
  await requestWithToken('/device/heartbeat', deviceToken, { method: 'POST', body: JSON.stringify({}) })
}

export async function deviceAck(deviceToken: string, configId?: string): Promise<void> {
  await requestWithToken('/device/ack', deviceToken, configId ? { method: 'POST', body: JSON.stringify({ config_id: configId }) } : { method: 'POST', body: JSON.stringify({}) })
}
