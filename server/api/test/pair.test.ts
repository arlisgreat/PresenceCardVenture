import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'
import { DemoStore } from '../src/demo-store.js'

const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xd9])

test('exposes a readiness probe that blocks incomplete production configuration', async () => {
  const demo = await buildApp({ uploadsDir: '/tmp/presence-card-test-ready-demo' })
  const demoReady = await demo.inject({ method: 'GET', url: '/health/ready' })
  assert.equal(demoReady.statusCode, 200)
  assert.equal(demoReady.json().status, 'ready')
  await demo.close()

  const production = await buildApp({ uploadsDir: '/tmp/presence-card-test-ready-production', requireProductionServices: true })
  const blocked = await production.inject({ method: 'GET', url: '/health/ready' })
  assert.equal(blocked.statusCode, 503)
  assert.equal(blocked.json().status, 'blocked')
  assert.ok(blocked.json().missing.includes('DATABASE_URL'))
  assert.ok(blocked.json().missing.includes('PERSISTENCE_PROVIDER'))
  await production.close()
})

test('production readiness stays blocked when Prisma is configured without a Prisma store adapter', async () => {
  const previous = {
    database: process.env.DATABASE_URL,
    bucket: process.env.OSS_BUCKET,
    ai: process.env.AI_PROVIDER,
    persistence: process.env.PERSISTENCE_PROVIDER,
  }
  process.env.DATABASE_URL = 'postgresql://postgres:secret@db:5432/presence'
  process.env.OSS_BUCKET = 'presence-production'
  process.env.AI_PROVIDER = 'replicate'
  process.env.PERSISTENCE_PROVIDER = 'prisma'
  try {
    const production = await buildApp({ uploadsDir: '/tmp/presence-card-test-ready-adapter', requireProductionServices: true })
    const blocked = await production.inject({ method: 'GET', url: '/health/ready' })
    assert.equal(blocked.statusCode, 503)
    assert.equal(blocked.json().checks.persistence_adapter, false)
    assert.equal(blocked.json().checks.session_adapter, false)
    assert.equal(blocked.json().checks.device_adapter, false)
    assert.equal(blocked.json().checks.device_token_encryption, false)
    assert.ok(blocked.json().missing.includes('PRISMA_STORE_ADAPTER'))
    assert.ok(blocked.json().missing.includes('PRISMA_SESSION_ADAPTER'))
    assert.ok(blocked.json().missing.includes('PRISMA_DEVICE_ADAPTER'))
    assert.ok(blocked.json().missing.includes('DEVICE_TOKEN_ENCRYPTION_KEY'))
    await production.close()
  } finally {
    if (previous.database === undefined) delete process.env.DATABASE_URL
    else process.env.DATABASE_URL = previous.database
    if (previous.bucket === undefined) delete process.env.OSS_BUCKET
    else process.env.OSS_BUCKET = previous.bucket
    if (previous.ai === undefined) delete process.env.AI_PROVIDER
    else process.env.AI_PROVIDER = previous.ai
    if (previous.persistence === undefined) delete process.env.PERSISTENCE_PROVIDER
    else process.env.PERSISTENCE_PROVIDER = previous.persistence
  }
})

test('pairs a device after a user binds its short-lived code', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-pair' })
  const deviceId = 'dvc_pair_test'
  const invalid = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: {} })
  assert.equal(invalid.statusCode, 400)
  assert.equal(invalid.json().error.code, 'BAD_REQUEST')
  const unauthorizedAck = await app.inject({ method: 'POST', url: '/v1/device/ack' })
  assert.equal(unauthorizedAck.statusCode, 401)
  const code = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: deviceId, fw_version: '0.1.0' } })
  assert.equal(code.statusCode, 200)
  const pairCode = code.json().pair_code
  assert.match(pairCode, /^\d{6}$/)
  const secondCode = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: 'dvc_pair_test_2' } })
  assert.equal(secondCode.statusCode, 200)
  assert.notEqual(secondCode.json().pair_code, pairCode)
  const pending = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=${pairCode}` })
  assert.equal(pending.statusCode, 202)
  const bind = await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, pair_code: pairCode } })
  assert.equal(bind.statusCode, 200)
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=${pairCode}` })
  assert.equal(status.statusCode, 200)
  assert.equal(status.json().status, 'bound')
  assert.ok(status.json().device_token)
  const deviceState = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(deviceState.statusCode, 200)
  const deviceAck = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(deviceAck.statusCode, 204)
  const deviceUpload = await app.inject({ method: 'POST', url: '/v1/photos', headers: { authorization: `Bearer ${status.json().device_token}`, 'content-type': 'image/jpeg', 'idempotency-key': 'pair-device-upload', 'x-device-id': deviceId, 'x-width': '320', 'x-height': '240' }, payload: jpeg })
  assert.equal(deviceUpload.statusCode, 201)
  const deviceDelete = await app.inject({ method: 'DELETE', url: `/v1/photos/${deviceUpload.json().photo_id}`, headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(deviceDelete.statusCode, 403)
  const userAck = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(userAck.statusCode, 401)
  await app.close()
})

test('device state maps pending friend requests to the device owner', async () => {
  const store = new DemoStore()
  store.users.set('u_outsider', { id: 'u_outsider', username: 'outsider', displayName: '旁观者', friendCode: '100009' })
  store.tokens.set('outsider-token', 'u_outsider')
  const app = await buildApp({ store, uploadsDir: '/tmp/presence-card-test-device-state' })
  const deviceId = 'dvc_state_test'
  const code = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: deviceId } })
  await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, pair_code: code.json().pair_code } })
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=${code.json().pair_code}` })
  const friendRequest = await app.inject({ method: 'POST', url: '/v1/friend-requests', headers: { authorization: 'Bearer outsider-token', 'content-type': 'application/json' }, payload: { friend_code: '100001' } })
  assert.equal(friendRequest.statusCode, 201)
  const state = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(state.json().pending_friend_requests, 1)
  await app.close()
})

test('queues a play config for the bound device and clears it on device ack', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-device-config' })
  const deviceId = 'dvc_config_test'
  const code = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: deviceId } })
  const pairCode = code.json().pair_code
  await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, pair_code: pairCode } })
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=${pairCode}` })
  const deviceToken = status.json().device_token

  const configHeaders = { authorization: 'Bearer demo-token', 'content-type': 'application/json', 'idempotency-key': 'config-retry-1' }
  const configPayload = { device_id: deviceId, filter_id: 'film', play_type: 'ccd', beauty: 28, sticker: 'star' }
  const queued = await app.inject({ method: 'POST', url: '/v1/device/config', headers: configHeaders, payload: configPayload })
  assert.equal(queued.statusCode, 202)
  assert.match(queued.json().config_id, /^cfg_/)
  const retry = await app.inject({ method: 'POST', url: '/v1/device/config', headers: configHeaders, payload: configPayload })
  assert.equal(retry.statusCode, 200)
  assert.equal(retry.json().config_id, queued.json().config_id)
  const state = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${deviceToken}` } })
  assert.deepEqual(state.json().pending_config, { id: queued.json().config_id, filter_id: 'film', play_type: 'ccd', beauty: 28, sticker: 'star', updated_at: state.json().pending_config.updated_at })

  const ack = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: `Bearer ${deviceToken}`, 'content-type': 'application/json' }, payload: { config_id: queued.json().config_id } })
  assert.equal(ack.statusCode, 204)
  const cleared = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${deviceToken}` } })
  assert.equal(cleared.json().pending_config, null)
  assert.deepEqual(cleared.json().active_config, { id: queued.json().config_id, filter_id: 'film', play_type: 'ccd', beauty: 28, sticker: 'star', updated_at: cleared.json().active_config.updated_at })
  await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: deviceId } })
  const afterPairCodeRefresh = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${deviceToken}` } })
  assert.equal(afterPairCodeRefresh.json().active_config.id, queued.json().config_id)

  const forbidden = await app.inject({ method: 'POST', url: '/v1/device/config', headers: { authorization: 'Bearer demo-user-2', 'content-type': 'application/json' }, payload: { device_id: deviceId, filter_id: 'warm', play_type: 'ccd', beauty: 0, sticker: 'none' } })
  assert.equal(forbidden.statusCode, 403)
  await app.close()
})

test('routes pairing through an injected persistent device store', async () => {
  let bound = false
  const calls: string[] = []
  const devicePairStore = {
    provider: 'prisma',
    savePairCode: async (deviceId: string, pairCode: string) => { calls.push(`${deviceId}:${pairCode}`) },
    bind: async () => { bound = true; return { deviceId: 'dvc_persistent', deviceToken: 'device-secret' } },
    status: async () => bound ? { status: 'bound' as const, deviceToken: 'device-secret', userId: 'u_demo_1' } : { status: 'pending' as const },
  }
  const app = await buildApp({
    uploadsDir: '/tmp/presence-card-test-persistent-device-route',
    devicePairStore,
    authStore: { provider: 'prisma', userForToken: async () => ({ id: 'u_demo_1', username: 'ayan', displayName: '阿岩', friendCode: '100001' }) },
  })
  const code = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: 'dvc_persistent', fw_version: '0.2.0' } })
  assert.equal(code.statusCode, 200)
  assert.equal(calls.length, 1)
  const pending = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=dvc_persistent&pair_code=${code.json().pair_code}` })
  assert.equal(pending.statusCode, 202)
  const bind = await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer real-token', 'content-type': 'application/json' }, payload: { device_id: 'dvc_persistent', pair_code: code.json().pair_code } })
  assert.equal(bind.statusCode, 200)
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=dvc_persistent&pair_code=${code.json().pair_code}` })
  assert.equal(status.statusCode, 200)
  assert.equal(status.json().device_token, 'device-secret')
  await app.close()
})
