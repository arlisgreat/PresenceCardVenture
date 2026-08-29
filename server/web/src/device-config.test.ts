import assert from 'node:assert/strict'
import test from 'node:test'
import { acknowledgeDeviceConfig, queueDeviceConfig, type DeviceConfigSyncState } from './device-config.js'

test('acknowledging a queued config keeps it as the active device config', () => {
  const initial: DeviceConfigSyncState = { active: null, pending: null }
  const queued = { id: 'cfg_1', name: '千禧 CCD' }

  const withPending = queueDeviceConfig(initial, queued)
  assert.deepEqual(withPending, { active: null, pending: queued })

  const acknowledged = acknowledgeDeviceConfig(withPending, queued.id)
  assert.deepEqual(acknowledged, { active: queued, pending: null })
})

test('an ack without a matching pending config does not erase the active config', () => {
  const active = { id: 'cfg_1', name: '千禧 CCD' }
  const state: DeviceConfigSyncState = { active, pending: null }

  assert.deepEqual(acknowledgeDeviceConfig(state, 'cfg_old'), state)
})
