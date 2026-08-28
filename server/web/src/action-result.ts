export async function runAction(action: () => Promise<unknown>, onError: () => void): Promise<boolean> {
  try {
    await action()
    return true
  } catch {
    onError()
    return false
  }
}
