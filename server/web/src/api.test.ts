import assert from 'node:assert/strict'
import test from 'node:test'
import { deviceAck, deviceHeartbeat, getPairStatus, uploadDevicePhoto } from './api.js'

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
