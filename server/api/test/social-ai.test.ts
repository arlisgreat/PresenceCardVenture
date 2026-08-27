import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

test('supports friends, reactions, and messages', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-social' })
  const auth = { authorization: 'Bearer demo-token' }
  const friends = await app.inject({ method: 'GET', url: '/v1/friends', headers: auth })
  assert.equal(friends.statusCode, 200)
  assert.ok(friends.json().length > 0)
  const request = await app.inject({ method: 'POST', url: '/v1/friend-requests', headers: { authorization: 'Bearer demo-user-2', 'content-type': 'application/json' }, payload: { friend_code: '100003' } })
  assert.equal(request.statusCode, 201)
  assert.equal(request.json().status, 'pending')
  const incoming = await app.inject({ method: 'GET', url: '/v1/friend-requests', headers: { authorization: 'Bearer demo-user-3' } })
  assert.equal(incoming.statusCode, 200)
  assert.equal(incoming.json().length, 1)
  const accepted = await app.inject({ method: 'POST', url: `/v1/friend-requests/${incoming.json()[0].id}/accept`, headers: { authorization: 'Bearer demo-user-3' } })
  assert.equal(accepted.statusCode, 200)
  assert.equal(accepted.json().status, 'accepted')
  const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers: auth })
  const photoId = feed.json().items[0].photo_id
  const reaction = await app.inject({ method: 'POST', url: `/v1/photos/${photoId}/reactions`, headers: { ...auth, 'content-type': 'application/json' }, payload: { type: 'heart' } })
  assert.equal(reaction.statusCode, 201)
  const cleared = await app.inject({ method: 'DELETE', url: `/v1/photos/${photoId}/reactions/heart`, headers: { ...auth, 'content-type': 'application/json' } })
  assert.equal(cleared.statusCode, 204)
  const conversation = await app.inject({ method: 'GET', url: '/v1/conversations', headers: auth })
  assert.equal(conversation.statusCode, 200)
  const message = await app.inject({ method: 'POST', url: '/v1/messages', headers: { ...auth, 'content-type': 'application/json' }, payload: { to: friends.json()[0].username, text: 'hello' } })
  assert.equal(message.statusCode, 201)
  const imageMessage = await app.inject({ method: 'POST', url: '/v1/messages', headers: { ...auth, 'content-type': 'application/json' }, payload: { to: friends.json()[0].username, photo_id: photoId } })
  assert.equal(imageMessage.statusCode, 201)
  assert.equal(imageMessage.json().kind, 'image')
  assert.equal(imageMessage.json().photo_id, photoId)
  const messages = await app.inject({ method: 'GET', url: `/v1/messages?friend=${friends.json()[0].username}`, headers: auth })
  assert.equal(messages.statusCode, 200)
  assert.equal(messages.json().find((item: any) => item.kind === 'image')?.photo_id, photoId)
  await app.close()
})

test('runs authorized AI jobs and rejects unauthorized materials', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/presence-card-test-ai' })
  const auth = { authorization: 'Bearer demo-token' }
  const materials = await app.inject({ method: 'GET', url: '/v1/photos/mine', headers: auth })
  const ids = materials.json().items.slice(0, 2).map((item: any) => item.photo_id)
  const created = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: { ...auth, 'content-type': 'application/json' }, payload: { material_ids: ids } })
  assert.equal(created.statusCode, 202)
  const jobId = created.json().job_id
  await new Promise((resolve) => setTimeout(resolve, 30))
  const status = await app.inject({ method: 'GET', url: `/v1/ai/jobs/${jobId}`, headers: auth })
  assert.ok(['processing', 'completed'].includes(status.json().status))
  const feed = await app.inject({ method: 'GET', url: '/v1/feed', headers: auth })
  const friendPhotoId = feed.json().items.find((item: any) => !item.mine).photo_id
  const noConsent = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: { ...auth, 'content-type': 'application/json' }, payload: { material_ids: [ids[0], friendPhotoId] } })
  assert.equal(noConsent.statusCode, 403)
  const consented = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: { ...auth, 'content-type': 'application/json' }, payload: { material_ids: [ids[0], friendPhotoId], consent: true } })
  assert.equal(consented.statusCode, 202)
  const forbidden = await app.inject({ method: 'POST', url: '/v1/ai/jobs', headers: { ...auth, 'content-type': 'application/json' }, payload: { material_ids: ['missing'] } })
  assert.equal(forbidden.statusCode, 403)
  await app.close()
})
