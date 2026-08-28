import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import path from 'node:path'
import test from 'node:test'

const schemaPath = path.resolve('prisma/schema.prisma')

function modelBlock(schema: string, name: string): string {
  const match = schema.match(new RegExp(`model ${name} \\{([\\s\\S]*?)\\n\\}`))
  assert.ok(match, `missing Prisma model ${name}`)
  return match[1]
}

test('Prisma schema covers web session and social/AI persistence contracts', async () => {
  const schema = await readFile(schemaPath, 'utf8')
  const session = modelBlock(schema, 'Session')
  assert.match(session, /userId\s+String/)
  assert.match(session, /tokenHash\s+String/)
  assert.match(session, /expiresAt\s+DateTime/)

  const message = modelBlock(schema, 'Message')
  assert.match(message, /fromId\s+String/)
  assert.match(message, /toId\s+String/)
  assert.match(message, /photoId\s+String\?/)

  const job = modelBlock(schema, 'AiJob')
  assert.match(job, /ownerId\s+String/)
  assert.match(job, /status\s+AiJobStatus/)
  assert.match(job, /materialIds\s+Json/)

  const photo = modelBlock(schema, 'Photo')
  for (const field of ['playType', 'beauty', 'sticker', 'circle', 'originalOssKey']) assert.match(photo, new RegExp(`^\\s*${field}\\s+`, 'm'))

  const device = modelBlock(schema, 'Device')
  assert.match(device, /tokenHash\s+String\?/)
  assert.match(device, /tokenCiphertext\s+String\?/)
  const idempotency = modelBlock(schema, 'IdempotencyKey')
  assert.match(idempotency, /photoId\s+String/)
  assert.match(idempotency, /photo\s+Photo/)
})

test('Prisma deployment assets include an initial migration and provider lock', async () => {
  const migrationPath = path.resolve('prisma/migrations/0001_initial_schema/migration.sql')
  const lockPath = path.resolve('prisma/migrations/migration_lock.toml')
  const [migration, lock] = await Promise.all([readFile(migrationPath, 'utf8'), readFile(lockPath, 'utf8')])
  assert.match(migration, /CREATE TABLE "sessions"/)
  assert.match(migration, /CREATE TABLE "messages"/)
  assert.match(migration, /CREATE TABLE "ai_jobs"/)
  assert.match(lock, /provider = "postgresql"/)
})
