export type DevicePairingActions<State> = {
  hardwarePairing: boolean
  bind: () => Promise<void>
  getStatus: () => Promise<{ device_token?: string }>
  saveToken: (deviceToken: string) => void
  heartbeat: (deviceToken: string) => Promise<void>
  getState: (deviceToken: string) => Promise<State>
}

export async function completeDevicePairing<State>(actions: DevicePairingActions<State>): Promise<
  { mode: 'hardware' } | { mode: 'simulator'; deviceToken: string; deviceState: State }
> {
  await actions.bind()
  if (actions.hardwarePairing) return { mode: 'hardware' }

  const status = await actions.getStatus()
  if (!status.device_token) throw new Error('device token missing')
  actions.saveToken(status.device_token)
  await actions.heartbeat(status.device_token)
  return {
    mode: 'simulator',
    deviceToken: status.device_token,
    deviceState: await actions.getState(status.device_token),
  }
}
