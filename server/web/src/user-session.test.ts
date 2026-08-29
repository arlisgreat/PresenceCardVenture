import assert from 'node:assert/strict'
import test from 'node:test'
import { canBindHardwarePairing, clearUserToken, fetchWithUserSession, getUserToken, setUserToken } from './user-session.js'

class MemoryStorage implements Storage {
  private readonly values = new Map<string, string>()
  get length() { return this.values.size }
  clear() { this.values.clear() }
  getItem(key: string) { return this.values.get(key) ?? null }
  key(index: number) { return [...this.values.keys()][index] ?? null }
  removeItem(key: string) { this.values.delete(key) }
  setItem(key: string, value: string) { this.values.set(key, value) }
}

function withLocalStorage(run: (storage: Storage) => Promise<void> | void) {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'localStorage')
  const storage = new MemoryStorage()
  Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: storage })
  return Promise.resolve(run(storage)).finally(() => {
    if (descriptor) Object.defineProperty(globalThis, 'localStorage', descriptor)
    else delete (globalThis as typeof globalThis & { localStorage?: Storage }).localStorage
  })
}

test('user session persists a selected token and clearing restores the demo fallback', () => withLocalStorage(storage => {
  assert.equal(getUserToken(), 'demo-token')
  setUserToken('  demo-user-2  ')
  assert.equal(storage.getItem('presence.user-token'), 'demo-user-2')
  assert.equal(getUserToken(), 'demo-user-2')
  setUserToken('   ')
  assert.equal(getUserToken(), 'demo-token')
  clearUserToken()
  assert.equal(storage.getItem('presence.user-token'), null)
}))

test('physical pairing rejects the public demo fallback but allows local or explicit sessions', () => withLocalStorage(() => {
  assert.equal(canBindHardwarePairing('presence.example.com'), false)
  assert.equal(canBindHardwarePairing('localhost'), true)
  assert.equal(canBindHardwarePairing('127.0.0.1'), true)
  assert.equal(canBindHardwarePairing('localhost.example.com'), false)

  setUserToken('demo-token')
  assert.equal(canBindHardwarePairing('presence.example.com'), false)
  setUserToken('signed-in-user-token')
  assert.equal(canBindHardwarePairing('presence.example.com'), true)
  clearUserToken()
  assert.equal(canBindHardwarePairing('presence.example.com'), false)
}))

test('authenticated fetch preserves request headers and uses the selected user token', () => withLocalStorage(async () => {
  const originalFetch = globalThis.fetch
  let request: { url: string; headers: Headers } | undefined
  globalThis.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    request = { url: String(input), headers: new Headers(init?.headers) }
    return new Response(null, { status: 204 })
  }) as typeof fetch
  try {
    setUserToken('demo-user-3')
    await fetchWithUserSession('/v1/me', { headers: { 'X-Request-Source': 'session-test' } })
    assert.equal(request?.url, '/v1/me')
    assert.equal(request?.headers.get('Authorization'), 'Bearer demo-user-3')
    assert.equal(request?.headers.get('X-Request-Source'), 'session-test')
  } finally {
    globalThis.fetch = originalFetch
  }
}))
