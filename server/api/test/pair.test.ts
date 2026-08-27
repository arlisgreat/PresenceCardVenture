import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

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
  await production.close()
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

test('queues a play config for the bound device and clears it on device ack', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-device-config' })
  const deviceId = 'dvc_config_test'
  const code = await app.inject({ method: 'POST', url: '/v1/pair/code', payload: { device_id: deviceId } })
  const pairCode = code.json().pair_code
  await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, pair_code: pairCode } })
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=${pairCode}` })
  const deviceToken = status.json().device_token

  const queued = await app.inject({ method: 'POST', url: '/v1/device/config', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, filter_id: 'film', play_type: 'ccd', beauty: 28, sticker: 'star' } })
  assert.equal(queued.statusCode, 202)
  assert.match(queued.json().config_id, /^cfg_/)
  const state = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${deviceToken}` } })
  assert.deepEqual(state.json().pending_config, { id: queued.json().config_id, filter_id: 'film', play_type: 'ccd', beauty: 28, sticker: 'star', updated_at: state.json().pending_config.updated_at })

  const ack = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: `Bearer ${deviceToken}`, 'content-type': 'application/json' }, payload: { config_id: queued.json().config_id } })
  assert.equal(ack.statusCode, 204)
  const cleared = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${deviceToken}` } })
  assert.equal(cleared.json().pending_config, null)

  const forbidden = await app.inject({ method: 'POST', url: '/v1/device/config', headers: { authorization: 'Bearer demo-user-2', 'content-type': 'application/json' }, payload: { device_id: deviceId, filter_id: 'warm', play_type: 'ccd', beauty: 0, sticker: 'none' } })
  assert.equal(forbidden.statusCode, 403)
  await app.close()
})
