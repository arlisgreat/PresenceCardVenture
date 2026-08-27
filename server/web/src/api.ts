export type PlayType = 'beauty' | 'ccd' | 'template'

export type FeedItem = {
  id: string
  photo_id?: string
  author: { username: string; display_name: string }
  filter_id: string
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
}

export type AiJob = {
  id: string
  status: 'queued' | 'processing' | 'completed' | 'failed'
  resultUrl?: string
  message?: string
}

const API = '/v1'

const demoFeed: FeedItem[] = [
  { id: 'seed-1', author: { username: 'momo', display_name: '墨墨' }, filter_id: 'warm', caption: '傍晚的风从窗台进来。', created_at: new Date(Date.now() - 1000 * 60 * 18).toISOString(), image_url: '/assets/feed-window.jpg', reactions: { heart: 12, star: 4 }, my_reactions: [], circle: '傍晚的天空' },
  { id: 'seed-2', author: { username: 'ayan', display_name: '阿岩' }, filter_id: 'film', caption: '把今天折成一张小卡。', created_at: new Date(Date.now() - 1000 * 60 * 72).toISOString(), image_url: '/assets/feed-portrait.jpg', reactions: { heart: 8, star: 2 }, my_reactions: [], circle: '小圈' },
  { id: 'seed-3', author: { username: 'luna', display_name: '露娜' }, filter_id: 'ccd', caption: '今天也有好好在场。', created_at: new Date(Date.now() - 1000 * 60 * 160).toISOString(), image_url: '/assets/feed-friends.jpg', reactions: { heart: 19, star: 7 }, my_reactions: [], circle: '宿舍窗台' },
]

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API}${path}`, { ...init, headers: { 'Content-Type': 'application/json', Authorization: 'Bearer demo-token', ...(init?.headers ?? {}) } })
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
    const response = await fetch(`${API}/photos`, { method: 'POST', body: file, headers: { Authorization: 'Bearer demo-token', 'Content-Type': file.type || 'image/jpeg', 'Idempotency-Key': `web-${Date.now()}`, 'X-Filter-Id': options.filterId, 'X-Caption': encodeURIComponent(options.caption), 'X-Width': '1080', 'X-Height': '1350' } })
    if (!response.ok) throw new Error(await response.text())
    const result = await response.json() as { photo_id: string; url?: string; created_at?: string }
    return { id: result.photo_id, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, caption: options.caption, created_at: result.created_at ?? new Date().toISOString(), image_url: result.url ?? localUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle: '我的小圈' }
  } catch {
    return { id: `local-${Date.now()}`, author: { username: 'me', display_name: '我' }, filter_id: options.filterId, caption: options.caption || '刚刚释放了一张照片。', created_at: new Date().toISOString(), image_url: localUrl, reactions: { heart: 0, star: 0 }, my_reactions: [], mine: true, circle: '我的小圈' }
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

export async function sendMessage(friend: string, body: string): Promise<Message> {
  try { return await request<Message>('/messages', { method: 'POST', body: JSON.stringify({ friend, body }) }) } catch {
    return { id: `m-${Date.now()}`, sender: 'me', senderName: '我', body, createdAt: new Date().toISOString() }
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

export async function getDeviceState(): Promise<{ unseen_count: number; pending_friend_requests: number; server_time: string }> {
  try { return await request('/device/state') } catch { return { unseen_count: 3, pending_friend_requests: 1, server_time: new Date().toISOString() } }
}
