import assert from 'node:assert/strict'
import test from 'node:test'
import { runAction } from './action-result.js'

test('reports successful async actions without invoking the error callback', async () => {
  let errors = 0
  const result = await runAction(async () => 'saved', () => { errors += 1 })
  assert.equal(result, true)
  assert.equal(errors, 0)
})

test('reports failed async actions and invokes the error callback exactly once', async () => {
  let errors = 0
  const result = await runAction(async () => { throw new Error('storage unavailable') }, () => { errors += 1 })
  assert.equal(result, false)
  assert.equal(errors, 1)
})
