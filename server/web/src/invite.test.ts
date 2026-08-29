import assert from 'node:assert/strict'
import test from 'node:test'
import { buildInviteUrl, readInviteCode } from './invite.js'

test('builds an origin-safe invite URL for sharing or QR encoding', () => {
  assert.equal(buildInviteUrl('100002', 'http://localhost:5173/'), 'http://localhost:5173/?join=100002')
  assert.equal(buildInviteUrl('100 002', 'https://presence.example'), 'https://presence.example/?join=100%20002')
})

test('reads only a bounded numeric invite code from a shared URL', () => {
  assert.equal(readInviteCode('?join=100002'), '100002')
  assert.equal(readInviteCode('?join=abc100002xyz'), '100002')
  assert.equal(readInviteCode('?join=123456789'), '123456')
  assert.equal(readInviteCode('?other=100002'), '')
})
