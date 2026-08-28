const SESSION_STORAGE_KEY = 'presence.user-token'
const env = (import.meta as ImportMeta & { env?: Record<string, string | undefined> }).env
const configuredToken = env?.VITE_PRESENCE_USER_TOKEN?.trim()
const fallbackToken = configuredToken || 'demo-token'

function browserStorage(): Storage | undefined {
  try {
    return globalThis.localStorage
  } catch {
    return undefined
  }
}

export function getUserToken(): string {
  const saved = browserStorage()?.getItem(SESSION_STORAGE_KEY)?.trim()
  return saved || fallbackToken
}

export function setUserToken(token: string): void {
  const storage = browserStorage()
  if (!storage) return
  const normalized = token.trim()
  if (normalized) storage.setItem(SESSION_STORAGE_KEY, normalized)
  else storage.removeItem(SESSION_STORAGE_KEY)
}

export function clearUserToken(): void {
  browserStorage()?.removeItem(SESSION_STORAGE_KEY)
}

export function fetchWithUserSession(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
  const headers = new Headers(init?.headers)
  headers.set('Authorization', `Bearer ${getUserToken()}`)
  return fetch(input, { ...init, headers, credentials: init?.credentials ?? 'same-origin' })
}

