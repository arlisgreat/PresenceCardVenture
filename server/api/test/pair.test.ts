import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

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
  assert.equal(code.json().pair_code, '482913')
  const pending = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=482913` })
  assert.equal(pending.statusCode, 202)
  const bind = await app.inject({ method: 'POST', url: '/v1/pair/bind', headers: { authorization: 'Bearer demo-token', 'content-type': 'application/json' }, payload: { device_id: deviceId, pair_code: '482913' } })
  assert.equal(bind.statusCode, 200)
  const status = await app.inject({ method: 'GET', url: `/v1/pair/status?device_id=${deviceId}&pair_code=482913` })
  assert.equal(status.statusCode, 200)
  assert.equal(status.json().status, 'bound')
  assert.ok(status.json().device_token)
  const deviceState = await app.inject({ method: 'GET', url: '/v1/device/state', headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(deviceState.statusCode, 200)
  const deviceAck = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: `Bearer ${status.json().device_token}` } })
  assert.equal(deviceAck.statusCode, 204)
  const userAck = await app.inject({ method: 'POST', url: '/v1/device/ack', headers: { authorization: 'Bearer demo-token' } })
  assert.equal(userAck.statusCode, 401)
  await app.close()
})
