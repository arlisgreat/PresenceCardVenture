export type DevicePairLink = { deviceId: string; pairCode: string }

const DEVICE_ID_PATTERN = /^dvc_[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/
const PAIR_CODE_PATTERN = /^\d{6}$/

export function readDevicePairLink(pathname: string, search: string): DevicePairLink | null {
  if (pathname.replace(/\/+$/, '') !== '/pair') return null
  const params = new URLSearchParams(search)
  const deviceId = params.get('device_id') ?? ''
  const pairCode = params.get('code') ?? ''
  if (!DEVICE_ID_PATTERN.test(deviceId) || !PAIR_CODE_PATTERN.test(pairCode)) return null
  return { deviceId, pairCode }
}

export function clearDevicePairLinkFromUrl(pairLink: DevicePairLink | null, replace: (path: string) => void): void {
  if (pairLink) replace('/pair')
}
