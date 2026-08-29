import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody, type Circle } from '../demo-store.js'

const auth = (r: FastifyRequest, s: DemoStore) => s.userForToken(String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, ''))

function circleBody(circle: Circle, store: DemoStore, userId: string) {
  return {
    id: circle.id,
    name: circle.name,
    type: circle.type,
    joined: store.subscriptionsFor(userId).has(circle.id),
    photo_count: [...store.photos.values()].filter(p => p.circleId === circle.id && !p.draftJobId).length,
    subscriber_count: [...store.circleSubscriptions.values()].filter(set => set.has(circle.id)).length,
  }
}

export async function circleRoutes(app: FastifyInstance, store: DemoStore) {
  app.get('/circles', async (r, reply) => {
    const u = auth(r, store)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const small: Circle = { id: 'c_small', name: '小圈', type: 'small', curatedPhotoIds: [] }
    const items = [circleBody(small, store, u.id), ...[...store.circles.values()].map(c => circleBody(c, store, u.id))]
    return { items }
  })

  app.post('/circles', async (r: any, reply) => {
    const u = auth(r, store)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const name = String(r.body?.name ?? '').trim()
    if (!name || name.length > 32) return reply.code(400).send(errorBody('BAD_REQUEST', 'circle name must be between 1 and 32 characters'))
    const existing = [...store.circles.values()].find(c => c.name === name)
    if (existing) return reply.code(409).send(errorBody('ALREADY_EXISTS', 'circle already exists'))
    const circle: Circle = { id: `c_${randomUUID()}`, name, type: 'big', ownerId: u.id, curatedPhotoIds: [] }
    store.circles.set(circle.id, circle)
    store.circleSubscriptions.set(u.id, new Set([...store.subscriptionsFor(u.id), circle.id]))
    return reply.code(201).send(circleBody(circle, store, u.id))
  })

  app.post('/circles/:id/join', async (r: any, reply) => {
    const u = auth(r, store)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const circle = store.circles.get(String(r.params.id))
    if (!circle) return reply.code(404).send(errorBody('NOT_FOUND', 'circle not found'))
    const subs = store.circleSubscriptions.get(u.id) ?? new Set<string>()
    subs.add(circle.id)
    store.circleSubscriptions.set(u.id, subs)
    return reply.code(200).send(circleBody(circle, store, u.id))
  })

  app.post('/circles/:id/leave', async (r: any, reply) => {
    const u = auth(r, store)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const circle = store.circles.get(String(r.params.id))
    if (!circle) return reply.code(404).send(errorBody('NOT_FOUND', 'circle not found'))
    store.circleSubscriptions.get(u.id)?.delete(circle.id)
    return reply.code(204).send()
  })

  app.get('/circles/:id/feed', async (r: any, reply) => {
    const u = auth(r, store)
    if (!u) return reply.code(401).send(errorBody('TOKEN_INVALID', 'token invalid'))
    const circleId = String(r.params.id)
    const rawLimit = (r.query as any)?.limit
    const parsedLimit = rawLimit === undefined ? 16 : Number(rawLimit)
    if (!Number.isInteger(parsedLimit) || parsedLimit < 1) return reply.code(400).send(errorBody('BAD_REQUEST', 'limit must be a positive integer'))
    const limit = Math.min(32, parsedLimit)
    const photos = circleId === 'c_small'
      ? [...store.photos.values()].filter(p => !p.draftJobId && !p.circleId && store.isFriend(u.id, p.authorId))
      : (() => {
          const circle = store.circles.get(circleId)
          if (!circle) return undefined
          if (!store.subscriptionsFor(u.id).has(circle.id)) return null
          return [...store.photos.values()].filter(p => !p.draftJobId && p.circleId === circle.id).sort((a, b) => b.createdAt.localeCompare(a.createdAt))
        })()
    if (photos === undefined) return reply.code(404).send(errorBody('NOT_FOUND', 'circle not found'))
    if (photos === null) return reply.code(403).send(errorBody('FORBIDDEN', 'join the circle to view its feed'))
    return { items: photos.slice(0, limit).map(p => ({
      photo_id: p.id, author: { username: store.user(p.authorId)?.username ?? '', display_name: store.user(p.authorId)?.displayName ?? '' },
      filter_id: p.filterId, play_type: p.playType ?? 'ccd', beauty: p.beauty ?? 0, sticker: p.sticker ?? 'none',
      caption: p.caption, circle: p.circle ?? '小圈', circle_id: p.circleId ?? null, created_at: p.createdAt,
      width: p.width, height: p.height, image_url: `/v1/photos/${p.id}/image`,
      reactions: store.reactionsFor(p.id), my_reactions: Object.keys(store.reactionsFor(p.id)).filter(type => store.reactions.has(`${p.id}:${type}:${u.id}`)), mine: p.authorId === u.id,
    })) }
  })
}
