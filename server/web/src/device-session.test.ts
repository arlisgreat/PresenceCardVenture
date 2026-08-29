import assert from 'node:assert/strict'
import test from 'node:test'
import { parseDeviceSession, serializeDeviceSession } from './device-session.js'

test('device session round-trips a device token', () => {
  const encoded = serializeDeviceSession({ deviceToken: 'device-token-test' })
  assert.deepEqual(parseDeviceSession(encoded), { deviceToken: 'device-token-test' })
})

test('invalid or incomplete device sessions are ignored', () => {
  assert.equal(parseDeviceSession('not-json'), null)
  assert.equal(parseDeviceSession(JSON.stringify({ deviceToken: '' })), null)
  assert.equal(parseDeviceSession(JSON.stringify({ deviceToken: 42 })), null)
})
