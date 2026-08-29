import { buildApp } from './app.js'
import { PrismaClient } from '@prisma/client'
import { PrismaSessionStore } from './prisma-session-store.js'
import { PrismaDeviceStore } from './prisma-device-store.js'
import { PrismaPhotoRepository } from './prisma-photo-repository.js'
import { createImageProviderFromEnv } from '@pvc/effects/providers'

const port = Number(process.env.PORT ?? 3000)
const usePrismaSessions = String(process.env.PERSISTENCE_PROVIDER ?? '').trim().toLowerCase() === 'prisma' && Boolean(process.env.DATABASE_URL)
const prisma = usePrismaSessions ? new PrismaClient() : undefined
const devicePairStore = prisma ? new PrismaDeviceStore(prisma, undefined, undefined, { encryptionKey: process.env.DEVICE_TOKEN_ENCRYPTION_KEY, requireEncryption: true }) : undefined
const photoMetadataRepository = prisma ? new PrismaPhotoRepository(prisma) : undefined
const configuredAiProvider = String(process.env.AI_PROVIDER ?? '').trim().toLowerCase()
const imageProvider = configuredAiProvider && !['simulator', 'demo', 'local'].includes(configuredAiProvider) ? createImageProviderFromEnv() : undefined

// Without an authoritative consent repository, other users' material stays blocked.
buildApp({ authStore: prisma ? new PrismaSessionStore(prisma) : undefined, devicePairStore, photoMetadataRepository, imageProvider }).then(async app => {
  if (prisma) app.addHook('onClose', async () => { await prisma.$disconnect() })
  await app.listen({ port, host: '0.0.0.0' })
}).catch(async err => {
  if (prisma) await prisma.$disconnect().catch(() => undefined)
  console.error(err)
  process.exit(1)
})
