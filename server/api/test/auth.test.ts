import test from 'node:test'
import assert from 'node:assert/strict'
import { buildApp } from '../src/app.js'

test('register creates an account, issues a session and enables login', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-auth-register' })
  const registered = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'nova', password: 'secret66', display_name: '新星' } })
  assert.equal(registered.statusCode, 201)
  const { token, user } = registered.json()
  assert.ok(token.startsWith('sess_'))
  assert.equal(user.username, 'nova')
  assert.match(user.friend_code, /^\d{6}$/)
  const me = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${token}` } })
  assert.equal(me.statusCode, 200)
  assert.equal(me.json().username, 'nova')

  const login = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { username: 'nova', password: 'secret66' } })
  assert.equal(login.statusCode, 200)
  assert.ok(login.json().token)
  const badLogin = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { username: 'nova', password: 'wrong-pass' } })
  assert.equal(badLogin.statusCode, 401)
  const dup = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'nova', password: 'secret66' } })
  assert.equal(dup.statusCode, 409)
  await app.close()
})

test('register validates username, password and invite code', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-auth-validate' })
  const badName = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'x', password: 'secret66' } })
  assert.equal(badName.statusCode, 400)
  const badPass = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'valid_name', password: '123' } })
  assert.equal(badPass.statusCode, 400)
  const badInvite = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'valid_name', password: 'secret66', invite_code: '000000' } })
  assert.equal(badInvite.statusCode, 404)
  await app.close()
})

test('registering with an invite code instantly connects inviter and newcomer', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-auth-invite' })
  const inviterMe = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: 'Bearer demo-token' } })
  const inviteCode = inviterMe.json().friend_code
  const registered = await app.inject({ method: 'POST', url: '/v1/auth/register', payload: { username: 'scanner', password: 'secret66', invite_code: inviteCode } })
  assert.equal(registered.statusCode, 201)
  const token = registered.json().token
  const friends = await app.inject({ method: 'GET', url: '/v1/friends', headers: { authorization: `Bearer ${token}` } })
  assert.equal(friends.statusCode, 200)
  assert.ok(friends.json().some((f: any) => f.username === 'ayan'))
  // inviter sees the newcomer too
  const inviterFriends = await app.inject({ method: 'GET', url: '/v1/friends', headers: { authorization: 'Bearer demo-token' } })
  assert.ok(inviterFriends.json().some((f: any) => f.username === 'scanner'))
  await app.close()
})

test('logout revokes the session token', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-auth-logout' })
  const login = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { username: 'momo', password: 'demo1234' } })
  assert.equal(login.statusCode, 200)
  const token = login.json().token
  const meBefore = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${token}` } })
  assert.equal(meBefore.statusCode, 200)
  const logout = await app.inject({ method: 'POST', url: '/v1/auth/logout', headers: { authorization: `Bearer ${token}` } })
  assert.equal(logout.statusCode, 204)
  const meAfter = await app.inject({ method: 'GET', url: '/v1/me', headers: { authorization: `Bearer ${token}` } })
  assert.equal(meAfter.statusCode, 401)
  await app.close()
})

test('seeded demo users can log in with the demo password', async () => {
  const app = await buildApp({ uploadsDir: '/tmp/pvc-auth-demo-login' })
  const login = await app.inject({ method: 'POST', url: '/v1/auth/login', payload: { username: 'ayan', password: 'demo1234' } })
  assert.equal(login.statusCode, 200)
  assert.equal(login.json().user.friend_code, '100001')
  await app.close()
})
