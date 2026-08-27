import type { FastifyInstance, FastifyRequest } from 'fastify'
import { randomUUID } from 'node:crypto'
import { DemoStore, errorBody } from '../demo-store.js'
const auth = (r: FastifyRequest, s: DemoStore) => s.userForToken(String(r.headers.authorization ?? '').replace(/^Bearer\s+/i, ''))
export async function socialRoutes(app: FastifyInstance, store: DemoStore) {
  app.get('/friends', async (r, reply) => { const u=auth(r,store); if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid')); return [...store.users.values()].filter(x=>x.id!==u.id&&store.isFriend(u.id,x.id)).map(x=>({username:x.username,display_name:x.displayName,since:new Date().toISOString()})) })
  app.post('/friends/claim', async (_r,reply)=>reply.code(501).send(errorBody('NOT_IMPLEMENTED','offline tickets are not implemented')))
  app.get('/conversations', async (r,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const peers=[...store.users.values()].filter(x=>x.id!==u.id&&store.isFriend(u.id,x.id));return peers.map(x=>({id:`conv_${[u.id,x.id].sort().join('_')}`,participant:{username:x.username,display_name:x.displayName},messages:store.messages.filter(m=>(m.from===u.id&&m.to===x.id)||(m.from===x.id&&m.to===u.id))}))})
  app.get('/conversations/:id/messages', async (r,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));return store.messages.filter(m=>m.from===u.id||m.to===u.id)})
  app.get('/messages', async (r:any,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const friend=r.query?.friend;const peer=friend?[...store.users.values()].find(x=>x.username===friend):undefined;return store.messages.filter(m=>(m.from===u.id||m.to===u.id)&&(!peer||(m.from===peer.id||m.to===peer.id))).map(m=>messageBody(m,store,u.id))})
  app.post('/messages', async (r,reply)=>{const u=auth(r,store), b:any=r.body;if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));const target=b?.to??b?.friend;const peer=[...store.users.values()].find(x=>x.username===target||x.id===target);if(!peer||!store.isFriend(u.id,peer.id))return reply.code(403).send(errorBody('FORBIDDEN','recipient is not a friend'));const text=b?.text??b?.body;const photoId=b?.photo_id;if(!text&&!photoId)return reply.code(400).send(errorBody('BAD_REQUEST','text or photo_id required'));if(photoId&&!store.photos.has(photoId))return reply.code(404).send(errorBody('NOT_FOUND','photo not found'));const m={id:`m_${randomUUID()}`,from:u.id,to:peer.id,text,photoId,createdAt:new Date().toISOString()};store.messages.push(m);return reply.code(201).send(messageBody(m,store,u.id))})
  app.post('/photos/:id/reactions', async (r,reply)=>{const u=auth(r,store),p=store.photos.get((r.params as any).id),type=(r.body as any)?.type;if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));if(!p)return reply.code(404).send(errorBody('NOT_FOUND','photo not found'));if(!['heart','thumbsup','wow'].includes(type))return reply.code(400).send(errorBody('BAD_REQUEST','invalid reaction'));store.reactions.set(`${p.id}:${type}:${u.id}`,new Set([u.id]));return reply.code(201).send({reactions:store.reactionsFor(p.id)})})
  app.delete('/photos/:id/reactions/:type', async (r,reply)=>{const u=auth(r,store);if(!u)return reply.code(401).send(errorBody('TOKEN_INVALID','token invalid'));store.reactions.delete(`${(r.params as any).id}:${(r.params as any).type}:${u.id}`);return reply.code(204).send()})
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
