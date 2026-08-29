export function buildInviteUrl(friendCode: string, origin: string): string {
  const base = origin.replace(/\/$/, '')
  return `${base}/?join=${encodeURIComponent(friendCode)}`
}

export function readInviteCode(search: string): string {
  return new URLSearchParams(search).get('join')?.replace(/\D/g, '').slice(0, 6) ?? ''
}
