import assert from 'node:assert/strict'
import test from 'node:test'
import { clearDevicePairLinkFromUrl, readDevicePairLink } from './device-pair-link.js'

test('reads a physical device pairing link', () => {
  assert.deepEqual(readDevicePairLink('/pair', '?device_id=dvc_a1b2c3d4e5f6&code=123456'), {
    deviceId: 'dvc_a1b2c3d4e5f6',
    pairCode: '123456',
  })
  assert.deepEqual(readDevicePairLink('/pair/', '?code=654321&device_id=dvc_display_01'), {
    deviceId: 'dvc_display_01',
    pairCode: '654321',
  })
})

test('rejects incomplete or malformed physical pairing links', () => {
  assert.equal(readDevicePairLink('/', '?device_id=dvc_a1b2c3&code=123456'), null)
  assert.equal(readDevicePairLink('/pair', '?device_id=dvc_a1b2c3'), null)
  assert.equal(readDevicePairLink('/pair', '?device_id=other_a1b2c3&code=123456'), null)
  assert.equal(readDevicePairLink('/pair', '?device_id=dvc_a1b2c3&code=12345x'), null)
  assert.equal(readDevicePairLink('/pair', '?device_id=dvc_a1b2c3%2Fbad&code=123456'), null)
})

test('clears pairing credentials from the address bar while preserving the pair path', () => {
  const replacements: string[] = []
  const link = readDevicePairLink('/pair', '?device_id=dvc_a1b2c3&code=123456')
  clearDevicePairLinkFromUrl(link, path => replacements.push(path))
  clearDevicePairLinkFromUrl(null, path => replacements.push(path))
  assert.deepEqual(replacements, ['/pair'])
})
