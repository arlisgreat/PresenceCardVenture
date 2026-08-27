import assert from 'node:assert/strict'
import test from 'node:test'
import { deviceAck, deviceHeartbeat, getPairStatus } from './api.js'

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
