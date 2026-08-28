export type DeviceSession = { deviceToken: string }

export function serializeDeviceSession(session: DeviceSession): string {
  return JSON.stringify(session)
}

export function parseDeviceSession(value: string | null): DeviceSession | null {
  if (!value) return null
  try {
    const parsed = JSON.parse(value) as { deviceToken?: unknown }
    return typeof parsed.deviceToken === 'string' && parsed.deviceToken.length > 0
      ? { deviceToken: parsed.deviceToken }
      : null
  } catch {
    return null
  }
}
