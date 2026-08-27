import assert from 'node:assert/strict'
import test from 'node:test'
import { createAiJob, deviceAck, deviceHeartbeat, filterFeedByCircle, getAiJob, getDeviceFeed, getPairStatus, uploadDevicePhoto, uploadPhoto } from './api.js'

test('device simulator helpers use the device token and preserve endpoint contracts', async () => {
  const calls: Array<{ url: string; method: string; authorization?: string }> = []
  const originalFetch = globalThis.fetch
  globalThis.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    calls.push({
      url: String(input),
      method: init?.method ?? 'GET',
      authorization: new Headers(init?.headers).get('Authorization') ?? undefined,
    })
    return new Response(JSON.stringify({ status: 'bound', device_token: 'device-token-test' }), { status: 200, headers: { 'Content-Type': 'application/json' } })
  }) as typeof fetch
  try {
    const status = await getPairStatus('dvc_test', '123456')
    assert.equal(status.device_token, 'device-token-test')
    await deviceHeartbeat(status.device_token)
    await deviceAck(status.device_token)
    assert.deepEqual(calls, [
      { url: '/v1/pair/status?device_id=dvc_test&pair_code=123456', method: 'GET', authorization: 'Bearer demo-token' },
      { url: '/v1/device/heartbeat', method: 'POST', authorization: 'Bearer device-token-test' },
      { url: '/v1/device/ack', method: 'POST', authorization: 'Bearer device-token-test' },
    ])
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('device photo upload helper sends JPEG bytes with device identity headers', async () => {
  let captured: { url: string; init?: RequestInit } | undefined
  const originalFetch = globalThis.fetch
  globalThis.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    captured = { url: String(input), init }
    return new Response(JSON.stringify({ photo_id: 'p_device_test', created_at: '2026-08-28T00:00:00.000Z' }), { status: 201, headers: { 'Content-Type': 'application/json' } })
  }) as typeof fetch
  try {
    const payload = new Blob([new Uint8Array([0xff, 0xd8, 0xff, 0xd9])], { type: 'image/jpeg' })
    const result = await uploadDevicePhoto(payload, 'device-token-test', 'dvc_test', 'device-idempotency-1')
    assert.equal(result.photo_id, 'p_device_test')
    assert.equal(captured?.url, '/v1/photos')
    assert.equal(captured?.init?.method, 'POST')
    const headers = new Headers(captured?.init?.headers)
    assert.equal(headers.get('Authorization'), 'Bearer device-token-test')
    assert.equal(headers.get('Content-Type'), 'image/jpeg')
    assert.equal(headers.get('X-Device-ID'), 'dvc_test')
    assert.equal(headers.get('Idempotency-Key'), 'device-idempotency-1')
    assert.equal(await new Response(captured?.init?.body).arrayBuffer().then(buffer => new Uint8Array(buffer)[0]), 0xff)
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('web photo upload includes the selected circle in metadata', async () => {
  const originalFetch = globalThis.fetch
  const originalCreateObjectUrl = URL.createObjectURL
  const originalRevokeObjectUrl = URL.revokeObjectURL
  let capturedHeaders: Headers | undefined
  globalThis.fetch = (async (_input: RequestInfo | URL, init?: RequestInit) => {
    capturedHeaders = new Headers(init?.headers)
    return new Response(JSON.stringify({ photo_id: 'p_circle_test', url: '/v1/photos/p_circle_test/image', created_at: '2026-08-28T00:00:00.000Z' }), { status: 201, headers: { 'Content-Type': 'application/json' } })
  }) as typeof fetch
  URL.createObjectURL = (() => 'blob:test-circle') as typeof URL.createObjectURL
  URL.revokeObjectURL = (() => {}) as typeof URL.revokeObjectURL
  try {
    const photo = await uploadPhoto(new File([new Uint8Array([0xff, 0xd8, 0xff, 0xd9])], 'capture.jpg', { type: 'image/jpeg' }), { filterId: 'warm', caption: '窗边', play: 'ccd', beauty: 12, sticker: 'none', circle: '傍晚的天空' })
    assert.equal(photo.circle, '傍晚的天空')
    assert.equal(capturedHeaders?.get('X-Circle'), encodeURIComponent('傍晚的天空'))
  } finally {
    globalThis.fetch = originalFetch
    URL.createObjectURL = originalCreateObjectUrl
    URL.revokeObjectURL = originalRevokeObjectUrl
  }
})

test('AI helpers propagate provider errors instead of returning fake success', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = (async () => new Response(JSON.stringify({ error: { code: 'AI_UNAVAILABLE' } }), { status: 503 })) as typeof fetch
  try {
    await assert.rejects(() => createAiJob(['p_1', 'p_2']), /AI_UNAVAILABLE|Request failed: 503/)
    await assert.rejects(() => getAiJob('job_1'), /AI_UNAVAILABLE|Request failed: 503/)
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('feed circle filtering keeps all view broad and named circles exact', () => {
  const feed = [
    { id: 'small', circle: '小圈' },
    { id: 'sky', circle: '傍晚的天空' },
    { id: 'film', circle: '胶片味' },
  ] as any[]
  assert.deepEqual(filterFeedByCircle(feed, '全部').map(item => item.id), ['small', 'sky', 'film'])
  assert.deepEqual(filterFeedByCircle(feed, '傍晚的天空').map(item => item.id), ['sky'])
  assert.deepEqual(filterFeedByCircle(feed, '小圈').map(item => item.id), ['small'])
})

test('device feed helper reads the authenticated device feed endpoint', async () => {
  const originalFetch = globalThis.fetch
  let request: { url: string; method?: string; authorization?: string } | undefined
  globalThis.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    request = { url: String(input), method: init?.method, authorization: new Headers(init?.headers).get('Authorization') ?? undefined }
    return new Response(JSON.stringify({ items: [{ photo_id: 'p_device_feed', author: { username: 'momo', display_name: '墨墨' }, filter_id: 'warm', image_url: '/v1/photos/p_device_feed/image', reactions: {} }] }), { status: 200, headers: { 'Content-Type': 'application/json' } })
  }) as typeof fetch
  try {
    const page = await getDeviceFeed('device-token-test', 8)
    assert.equal(page.items[0].photo_id, 'p_device_feed')
    assert.deepEqual(request, { url: '/v1/feed?limit=8', method: 'GET', authorization: 'Bearer device-token-test' })
  } finally {
    globalThis.fetch = originalFetch
  }
})
