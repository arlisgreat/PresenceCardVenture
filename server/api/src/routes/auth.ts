import type { FastifyInstance } from 'fastify'
import { randomInt, randomUUID } from 'node:crypto'
import { DemoStore, errorBody, hashPassword, verifyPassword } from '../demo-store.js'

const bearer = (headers: Record<string, unknown>) => String(headers.authorization ?? '').replace(/^Bearer\s+/i, '')

const USERNAME_PATTERN = /^[a-zA-Z0-9_]{2,24}$/

export async function authRoutes(app: FastifyInstance, store: DemoStore) {
  app.post('/auth/register', async (r: any, reply) => {
    const body = r.body ?? {}
    const username = String(body.username ?? '').trim()
    const password = String(body.password ?? '')
    const displayName = String(body.display_name ?? '').trim() || username
    const inviteCode = String(body.invite_code ?? '').replace(/\D/g, '')
    if (!USERNAME_PATTERN.test(username)) return reply.code(400).send(errorBody('BAD_REQUEST', 'username must be 2-24 letters, digits or underscores'))
    if (password.length < 6 || password.length > 128) return reply.code(400).send(errorBody('BAD_REQUEST', 'password must be 6-128 characters'))
    if (displayName.length > 32) return reply.code(400).send(errorBody('BAD_REQUEST', 'display_name must be at most 32 characters'))
    if ([...store.users.values()].some(u => u.username === username)) return reply.code(409).send(errorBody('ALREADY_EXISTS', 'username is taken'))

    let inviter = undefined as undefined | { id: string }
    if (inviteCode) {
      if (inviteCode.length !== 6) return reply.code(400).send(errorBody('BAD_REQUEST', 'invite_code must be 6 digits'))
      const found = [...store.users.values()].find(u => u.friendCode === inviteCode)
      if (!found) return reply.code(404).send(errorBody('NOT_FOUND', 'invite code not found'))
      inviter = { id: found.id }
    }

    const { salt, hash } = hashPassword(password)
    const friendCode = await uniqueFriendCode(store)
    const user = { id: `u_${randomUUID()}`, username, displayName, friendCode, passwordSalt: salt, passwordHash: hash }
    store.users.set(user.id, user)

    if (inviter) {
      store.friendships.add(`${inviter.id}:${user.id}`)
      store.friendships.add(`${user.id}:${inviter.id}`)
    }

    const session = store.createSession(user.id)
    return reply.code(201).send({ token: session.token, expires_in: 72 * 3600, user: { id: user.id, username: user.username, display_name: user.displayName, friend_code: user.friendCode } })
  })

  app.post('/auth/login', async (r: any, reply) => {
    const body = r.body ?? {}
    const username = String(body.username ?? '').trim()
    const password = String(body.password ?? '')
    const user = [...store.users.values()].find(u => u.username === username)
    if (!user || !user.passwordSalt || !user.passwordHash || !verifyPassword(password, user.passwordSalt, user.passwordHash)) {
      return reply.code(401).send(errorBody('AUTH_FAILED', 'username or password is incorrect'))
    }
    const session = store.createSession(user.id)
    return { token: session.token, expires_in: 72 * 3600, user: { id: user.id, username: user.username, display_name: user.displayName, friend_code: user.friendCode } }
  })

  app.post('/auth/logout', async (r, reply) => {
    const token = bearer(r.headers)
    if (token) store.revokeSession(token)
    return reply.code(204).send()
  })
}

async function uniqueFriendCode(store: DemoStore): Promise<string> {
  for (let attempt = 0; attempt < 32; attempt++) {
    const code = String(randomInt(100000, 1000000))
    if (![...store.users.values()].some(u => u.friendCode === code)) return code
  }
  throw new Error('friend code space exhausted')
}
