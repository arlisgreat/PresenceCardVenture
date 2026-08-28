import { buildApp } from './app.js'
import { PrismaClient } from '@prisma/client'
import { PrismaSessionStore } from './prisma-session-store.js'

const port = Number(process.env.PORT ?? 3000)
const usePrismaSessions = String(process.env.PERSISTENCE_PROVIDER ?? '').trim().toLowerCase() === 'prisma' && Boolean(process.env.DATABASE_URL)
const prisma = usePrismaSessions ? new PrismaClient() : undefined

buildApp({ authStore: prisma ? new PrismaSessionStore(prisma) : undefined }).then(async app => {
  if (prisma) app.addHook('onClose', async () => { await prisma.$disconnect() })
  await app.listen({ port, host: '0.0.0.0' })
}).catch(async err => {
  if (prisma) await prisma.$disconnect().catch(() => undefined)
  console.error(err)
  process.exit(1)
})
