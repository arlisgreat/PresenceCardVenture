import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody } from '../demo-store.js'
const auth = (r: FastifyRequest, s: DemoStore) => s.userForToken(String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, ''))
// Reactions are a device API too (docs/02 §3.4): resolve device tokens to the paired user.
const reactionActor = (r: FastifyRequest, s: DemoStore) => {
  const token = String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, '')
  const user = s.userForToken(token)
  if (user) return user
  const device = s.deviceForToken(token)
  return device?.userId ? s.user(device.userId) : undefined
}
export async function socialRoutes(app: FastifyInstance, store: DemoStore) {
  // Private AI drafts cannot escape via messages or reaction activity before publish.
  app.addHook('preHandler', async (request, reply) => {
    const body = request.body as { photo_id?: string } | undefined
    const params = request.params as { id?: string } | undefined
    const routePath = request.routeOptions.url ?? ''
    const id = routePath.includes('/photos/') && routePath.includes('/reactions') ? params?.id : (routePath === '/v1/messages' || routePath === '/v1/reactions') ? body?.photo_id : undefined
    if (id && store.photos.get(id)?.draftJobId) return reply.code(403).send(errorBody('FORBIDDEN', 'AI draft is private'))
  })
  app.get('/friends', async (r, reply) => { const u=auth(r,store); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); return [...store.users.values()].filter(x=>x.id!==u.id&&store.isFriend(u.id,x.id)).map(x=>({username:x.username,display_name:x.displayName,since:new Date().toISOString()})) })
  app.get('/friend-requests', async (r, reply) => { const u=auth(r,store); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); return [...store.friendRequests.values()].filter(x=>x.status==='pending'&&(x.requesterId===u.id||x.addresseeId===u.id)).map(x=>friendRequestBody(x,store,u.id)) })
  app.post('/friend-requests', async (r:any, reply) => { const u=auth(r,store), code=String(r.body?.friend_code??'').trim(); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); const target=[...store.users.values()].find(x=>x.friendCode===code); if(!target)return reply.code(404).send(errorBody('NOT_FOUND','friend code not found')); if(target.id===u.id)return reply.code(400).send(errorBody('BAD_REQUEST','cannot add yourself')); if(store.isFriend(u.id,target.id))return reply.code(409).send(errorBody('ALREADY_EXISTS','already friends')); const existing=[...store.friendRequests.values()].find(x=>x.status==='pending'&&((x.requesterId===u.id&&x.addresseeId===target.id)||(x.requesterId===target.id&&x.addresseeId===u.id))); if(existing)return reply.code(409).send(errorBody('ALREADY_EXISTS','friend request already pending')); const item={id:`fr_${randomUUID()}`,requesterId:u.id,addresseeId:target.id,status:'pending' as const,createdAt:new Date().toISOString()}; store.friendRequests.set(item.id,item); return reply.code(201).send(friendRequestBody(item,store,u.id)) })
  app.post('/friend-requests/:id/accept', async (r:any, reply) => { const u=auth(r,store), item=store.friendRequests.get(r.params.id); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); if(!item)return reply.code(404).send(errorBody('NOT_FOUND','friend request not found')); if(item.addresseeId!==u.id)return reply.code(403).send(errorBody('FORBIDDEN','only the recipient can accept')); if(item.status!=='pending')return reply.code(409).send(errorBody('ALREADY_EXISTS','friend request already handled')); item.status='accepted'; store.friendships.add(`${item.requesterId}:${item.addresseeId}`); store.friendships.add(`${item.addresseeId}:${item.requesterId}`); return reply.send(friendRequestBody(item,store,u.id)) })
  app.post('/friends/claim', async (_r,reply)=>reply.code(501).send(errorBody('NOT_IMPLEMENTED','offline tickets are not implemented')))
  app.get('/conversations', async (r,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const peers=[...store.users.values()].filter(x=>x.id!==u.id&&store.isFriend(u.id,x.id));return peers.map(x=>({id:`conv_${[u.id,x.id].sort().join('_')}`,participant:{username:x.username,display_name:x.displayName},messages:store.messages.filter(m=>(m.from===u.id&&m.to===x.id)||(m.from===x.id&&m.to===u.id))}))})
  app.get('/conversations/:id/messages', async (r:any,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const peers=[...store.users.values()].filter(x=>x.id!==u.id&&store.isFriend(u.id,x.id));const peer=peers.find(x=>`conv_${[u.id,x.id].sort().join('_')}`===r.params.id);if(!peer)return reply.code(404).send(errorBody('NOT_FOUND','conversation not found'));return store.messages.filter(m=>(m.from===u.id&&m.to===peer.id)||(m.from===peer.id&&m.to===u.id)).map(m=>messageBody(m,store,u.id))})
  app.get('/messages', async (r:any,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const friend=r.query?.friend;const peer=friend?[...store.users.values()].find(x=>x.username===friend):undefined;return store.messages.filter(m=>(m.from===u.id||m.to===u.id)&&(!peer||(m.from===peer.id||m.to===peer.id))).map(m=>messageBody(m,store,u.id))})
  app.post('/messages', async (r,reply)=>{const u=auth(r,store), b:any=r.body;if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const target=b?.to??b?.friend;const peer=[...store.users.values()].find(x=>x.username===target||x.id===target);if(!peer||!store.isFriend(u.id,peer.id))return reply.code(403).send(errorBody('FORBIDDEN','recipient is not a friend'));const text=b?.text??b?.body;const photoId=b?.photo_id;if(!text&&!photoId)return reply.code(400).send(errorBody('BAD_REQUEST','text or photo_id required'));if(photoId&&!store.photos.has(photoId))return reply.code(404).send(errorBody('NOT_FOUND','photo not found'));const m={id:`m_${randomUUID()}`,from:u.id,to:peer.id,text,photoId,createdAt:new Date().toISOString()};store.messages.push(m);return reply.code(201).send(messageBody(m,store,u.id))})
  app.post('/photos/:id/reactions', async (r,reply)=>{const u=reactionActor(r,store),p=store.photos.get((r.params as any).id),type=(r.body as any)?.type;if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));if(!p)return reply.code(404).send(errorBody('NOT_FOUND','photo not found'));if(!['heart','thumbsup','wow'].includes(type))return reply.code(400).send(errorBody('BAD_REQUEST','invalid reaction'));store.reactions.set(`${p.id}:${type}:${u.id}`,new Set([u.id]));return reply.code(201).send({reactions:store.reactionsFor(p.id)})})
  app.delete('/photos/:id/reactions/:type', async (r,reply)=>{const u=reactionActor(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));store.reactions.delete(`${(r.params as any).id}:${(r.params as any).type}:${u.id}`);return reply.code(204).send()})
  // Display-frame firmware compat: aggregated taps arrive as POST /v1/reactions
  // {photo_id, tap_count} with a device token; fold them into a heart reaction.
  app.post('/reactions', async (r,reply)=>{const u=reactionActor(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const p=store.photos.get(String((r.body as any)?.photo_id??''));if(!p)return reply.code(404).send(errorBody('NOT_FOUND','photo not found'));store.reactions.set(`${p.id}:heart:${u.id}`,new Set([u.id]));return reply.code(201).send({reactions:store.reactionsFor(p.id)})})
}

function messageBody(message: any, store: DemoStore, userId: string) {
  return {
    id: message.id,
    sender: message.from === userId ? 'me' : store.user(message.from)?.username,
    senderName: message.from === userId ? '我' : store.user(message.from)?.displayName,
    body: message.text ?? '',
    createdAt: message.createdAt,
    kind: message.photoId ? 'image' : 'text',
    photo_id: message.photoId,
    image_url: message.photoId ? `/v1/photos/${message.photoId}/image` : undefined,
  }
}

function friendRequestBody(item: any, store: DemoStore, userId: string) {
  const requester = store.user(item.requesterId)
  const addressee = store.user(item.addresseeId)
  return { id: item.id, status: item.status, direction: item.requesterId === userId ? 'outgoing' : 'incoming', requester: { username: requester?.username, display_name: requester?.displayName }, addressee: { username: addressee?.username, display_name: addressee?.displayName }, created_at: item.createdAt }
}
