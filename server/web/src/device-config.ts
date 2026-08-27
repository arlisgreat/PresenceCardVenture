export type DeviceConfigLabel = { id: string; name: string }

export type DeviceConfigSyncState = {
  active: DeviceConfigLabel | null
  pending: DeviceConfigLabel | null
}

export function queueDeviceConfig(state: DeviceConfigSyncState, config: DeviceConfigLabel): DeviceConfigSyncState {
  return { ...state, pending: config }
}

export function acknowledgeDeviceConfig(state: DeviceConfigSyncState, configId?: string): DeviceConfigSyncState {
  if (!state.pending || (configId && state.pending.id !== configId)) return state
  return { active: state.pending, pending: null }
}
