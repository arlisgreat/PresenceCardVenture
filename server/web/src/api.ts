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

export type AiJob = {
  id: string
  status: 'queued' | 'processing' | 'completed' | 'failed'
  resultUrl?: string
  message?: string
}

class ApiError extends Error {
  constructor(public readonly status: number, message: string) {
    super(message)
    this.name = 'ApiError'
  }
}

const API = '/v1'

async function jpegPayload(file: File): Promise<Blob | File> {
  if (file.type === 'image/jpeg') return file
  if (file.type !== 'image/png') throw new Error('仅支持 JPG 或 PNG 图片')
  if (typeof createImageBitmap !== 'function') return file
  const bitmap = await createImageBitmap(file)
  const canvas = document.createElement('canvas')
  const scale = Math.min(1, 1600 / Math.max(bitmap.width, bitmap.height))
  canvas.width = Math.max(1, Math.round(bitmap.width * scale))
  canvas.height = Math.max(1, Math.round(bitmap.height * scale))
  canvas.getContext('2d')?.drawImage(bitmap, 0, 0, canvas.width, canvas.height)
  bitmap.close()
  const blob = await new Promise<Blob | null>(resolve => canvas.toBlob(resolve, 'image/jpeg', .88))
  if (!blob) throw new Error('图片转换失败')
  return blob
}

const demoFeed: FeedItem[] = [
  { id: 'seed-1', author: { username: 'momo', display_name: '墨墨' }, filter_id: 'warm', caption: '傍晚的风从窗台进来。', created_at: new Date(Date.now() - 1000 * 60 * 18).toISOString(), image_url: '/assets/feed-window.jpg', reactions: { heart: 12, star: 4 }, my_reactions: [], circle: '傍晚的天空' },
  { id: 'seed-2', author: { username: 'ayan', display_name: '阿岩' }, filter_id: 'film', caption: '把今天折成一张小卡。', created_at: new Date(Date.now() - 1000 * 60 * 72).toISOString(), image_url: '/assets/feed-portrait.jpg', reactions: { heart: 8, star: 2 }, my_reactions: [], circle: '小圈' },
  { id: 'seed-3', author: { username: 'luna', display_name: '露娜' }, filter_id: 'ccd', caption: '今天也有好好在场。', created_at: new Date(Date.now() - 1000 * 60 * 160).toISOString(), image_url: '/assets/feed-friends.jpg', reactions: { heart: 19, star: 7 }, my_reactions: [], circle: '宿舍窗台' },
]

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const headers: Record<string, string> = { Authorization: 'Bearer demo-token', ...(init?.headers as Record<string, string> ?? {}) }
  if (init?.body) headers['Content-Type'] = 'application/json'
  const response = await fetch(`${API}${path}`, { ...init, headers })
  if (!response.ok) throw new Error((await response.text()) || `Request failed: ${response.status}`)
  if (response.status === 204) return undefined as T
  return response.json() as Promise<T>
}

export async function getFeed(): Promise<FeedItem[]> {
  try {
    const data = await request<{ items?: FeedItem[] }>('/feed?limit=20')
    return (data.items ?? []).map((item, index) => item.photo_id?.startsWith('p_demo_') ? { ...item, id: item.photo_id, image_url: demoFeed[index % demoFeed.length].image_url } : { ...item, id: item.photo_id ?? item.id })
  } catch {
    return demoFeed
  }
}

export async function uploadPhoto(file: File, options: { filterId: string; caption: string; play: PlayType; beauty: number; sticker: string }): Promise<FeedItem> {
  const localUrl = URL.createObjectURL(file)
  try {
    const payload = await jpegPayload(file)
    const response = await fetch(`${API}/photos`, { method: 'POST', body: payload, headers: { Authorization: 'Bearer demo-token', 'Content-Type': 'image/jpeg', 'Idempotency-Key': `web-${Date.now()}`, 'X-Filter-Id': options.filterId, 'X-Play-Type': options.play, 'X-Beauty': String(options.beauty), 'X-Sticker': options.sticker, 'X-Caption': encodeURIComponent(options.caption), 'X-Width': '1080', 'X-Height': '1350' } })
    if (!response.ok) throw new ApiError(response.status, (await response.text()) || `Request failed: ${response.status}`)
    const result = await response.json() as { photo_id: string; url?: string; created_at?: string }
    const imageUrl = result.url ?? localUrl
    if (result.url) URL.revokeObjectURL(localUrl)
    return { id: result.photo_id, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, play_type: options.play, beauty: options.beauty, sticker: options.sticker, caption: options.caption, created_at: result.created_at ?? new Date().toISOString(), image_url: imageUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle: '我的小圈' }
  } catch (error) {
    if (!(error instanceof TypeError)) throw error
    return { id: `local-${Date.now()}`, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, play_type: options.play, beauty: options.beauty, sticker: options.sticker, caption: options.caption || '刚刚释放了一张照片。', created_at: new Date().toISOString(), image_url: localUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle: '我的小圈' }
  }
}

export async function reactToPhoto(id: string, active: boolean): Promise<void> {
  try {
    if (active) await request(`/photos/${id}/reactions`, { method: 'POST', body: JSON.stringify({ type: 'heart' }) })
    else await request(`/photos/${id}/reactions/heart`, { method: 'DELETE' })
  } catch { /* local fallback is handled by the view */ }
}

export async function pokePhoto(id: string): Promise<void> {
  try { await request(`/photos/${id}/reactions`, { method: 'POST', body: JSON.stringify({ type: 'wow' }) }) } catch { /* the toast still gives local feedback */ }
}

export async function deletePhoto(id: string): Promise<void> {
  if (id.startsWith('local-') || id.startsWith('seed-')) return
  await request(`/photos/${id}`, { method: 'DELETE' })
}

export async function getMessages(friend = 'luna'): Promise<Message[]> {
  try { return await request<Message[]>(`/messages?friend=${encodeURIComponent(friend)}`) } catch {
    return [
      { id: 'm1', sender: 'luna', senderName: '露娜', body: '今天的光线好漂亮，像一张旧相纸。', createdAt: new Date(Date.now() - 1000 * 60 * 42).toISOString() },
      { id: 'm2', sender: 'me', senderName: '我', body: '我也拍下来了，晚点放进圈子。', createdAt: new Date(Date.now() - 1000 * 60 * 37).toISOString() },
    ]
  }
}

export async function getFriends(): Promise<Friend[]> {
  try { return await request<Friend[]>('/friends') } catch { return [{ username: 'luna', display_name: '露娜' }, { username: 'momo', display_name: '墨墨' }] }
}

export async function getFriendRequests(): Promise<FriendRequest[]> {
  try { return await request<FriendRequest[]>('/friend-requests') } catch { return [] }
}

export async function sendFriendRequest(friendCode: string): Promise<FriendRequest> {
  return request<FriendRequest>('/friend-requests', { method: 'POST', body: JSON.stringify({ friend_code: friendCode }) })
}

export async function acceptFriendRequest(id: string): Promise<FriendRequest> {
  return request<FriendRequest>(`/friend-requests/${id}/accept`, { method: 'POST', body: JSON.stringify({}) })
}

export async function sendMessage(friend: string, body: string, photoId?: string): Promise<Message> {
  try { return await request<Message>('/messages', { method: 'POST', body: JSON.stringify({ friend, body, photo_id: photoId }) }) } catch {
    return { id: `m-${Date.now()}`, sender: 'me', senderName: '我', body, createdAt: new Date().toISOString(), kind: photoId ? 'image' : 'text', photo_id: photoId, image_url: photoId && !photoId.startsWith('local-') ? `/v1/photos/${photoId}/image` : undefined }
  }
}

export async function createAiJob(photoIds: string[]): Promise<AiJob> {
  try {
    const response = await request<AiJob & { job_id?: string }>('/ai/jobs', { method: 'POST', body: JSON.stringify({ photo_ids: photoIds, consent: true }) })
    return { ...response, id: response.id ?? response.job_id ?? `ai-${Date.now()}` }
  } catch {
    const id = `ai-${Date.now()}`
    return { id, status: 'queued', message: '素材已授权，正在合成一张只属于你们的合照。' }
  }
}

export async function getAiJob(id: string): Promise<AiJob> {
  try {
    const response = await request<AiJob & { result_photo_id?: string; error?: string }>(`/ai/jobs/${id}`)
    return { id, status: response.status, resultUrl: response.resultUrl ?? (response as AiJob & { result_url?: string }).result_url ?? (response.status === 'completed' ? demoFeed[2].image_url : undefined), message: response.message ?? response.error }
  } catch {
    return { id, status: 'completed', resultUrl: demoFeed[2].image_url, message: '合照已经生成。' }
  }
}

export async function deleteAiJob(id: string): Promise<void> {
  if (id.startsWith('ai-')) return
  await request(`/ai/jobs/${id}/result`, { method: 'DELETE' })
}

export async function getDeviceState(): Promise<{ unseen_count: number; pending_friend_requests: number; server_time: string }> {
  try { return await request('/device/state') } catch { return { unseen_count: 3, pending_friend_requests: 1, server_time: new Date().toISOString() } }
}

export async function requestPairCode(deviceId: string): Promise<{ pair_code: string; expires_in: number }> {
  return request('/pair/code', { method: 'POST', body: JSON.stringify({ device_id: deviceId, fw_version: '0.1.0', hw: 'cams3-lite' }) })
}

export async function bindDevice(deviceId: string, pairCode: string): Promise<void> {
  await request('/pair/bind', { method: 'POST', body: JSON.stringify({ device_id: deviceId, pair_code: pairCode }) })
}
