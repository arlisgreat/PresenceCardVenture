import { createHash } from 'node:crypto'
import type { User } from './demo-store.js'
import type { DemoStore } from './demo-store.js'

type SessionUser = {
  id: string
  username: string
  displayName: string
  friendCode: string
}

type SessionRecord = {
  tokenHash: string
  expiresAt: Date
  revokedAt: Date | null
  user: SessionUser
}

export type SessionClient = {
  session: {
    findUnique(args: { where: { tokenHash: string }; include: { user: true }}): Promise<SessionRecord | null>
  }
}

export interface UserSessionStore {
  readonly provider: string
  userForToken(token?: string): Promise<User | undefined>
}

const tokenHash = (token: string) => createHash('sha256').update(token).digest('hex')

/** Resolves web sessions without ever persisting or querying the bearer secret. */
export class PrismaSessionStore implements UserSessionStore {
  readonly provider = 'prisma' as const

  constructor(private readonly client: SessionClient, private readonly now: () => Date = () => new Date()) {}

  async userForToken(token?: string): Promise<User | undefined> {
    const secret = String(token ?? '').trim()
    if (!secret) return undefined
    const session = await this.client.session.findUnique({
      where: { tokenHash: tokenHash(secret) },
      include: { user: true },
    })
    if (!session || session.revokedAt || session.expiresAt.getTime() <= this.now().getTime()) return undefined
    return {
      id: session.user.id,
      username: session.user.username,
      displayName: session.user.displayName,
      friendCode: session.user.friendCode,
    }
  }
}

export class DemoSessionStore implements UserSessionStore {
  readonly provider = 'demo' as const

  constructor(private readonly store: DemoStore) {}

  async userForToken(token?: string): Promise<User | undefined> {
    return this.store.userForToken(token)
  }
}
