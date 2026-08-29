import assert from 'node:assert/strict'
import test from 'node:test'
import { completeDevicePairing } from './device-pairing.js'

test('physical hardware pairing performs only the user-side bind', async () => {
  const calls: string[] = []
  const result = await completeDevicePairing({
    hardwarePairing: true,
    bind: async () => { calls.push('bind') },
    getStatus: async () => { calls.push('status'); return { device_token: 'must-not-be-read' } },
    saveToken: () => { calls.push('save') },
    heartbeat: async () => { calls.push('heartbeat') },
    getState: async () => { calls.push('state'); return {} },
  })

  assert.deepEqual(result, { mode: 'hardware' })
  assert.deepEqual(calls, ['bind'])
})

test('device simulator retains token, heartbeat, and state flow', async () => {
  const calls: string[] = []
  const result = await completeDevicePairing({
    hardwarePairing: false,
    bind: async () => { calls.push('bind') },
    getStatus: async () => { calls.push('status'); return { device_token: 'device-token-test' } },
    saveToken: token => { calls.push(`save:${token}`) },
    heartbeat: async token => { calls.push(`heartbeat:${token}`) },
    getState: async token => { calls.push(`state:${token}`); return { pending: 1 } },
  })

  assert.deepEqual(result, { mode: 'simulator', deviceToken: 'device-token-test', deviceState: { pending: 1 } })
  assert.deepEqual(calls, [
    'bind',
    'status',
    'save:device-token-test',
    'heartbeat:device-token-test',
    'state:device-token-test',
  ])
})
