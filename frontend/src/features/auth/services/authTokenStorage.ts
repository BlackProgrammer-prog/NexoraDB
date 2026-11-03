// Admin bearer tokens live only in this tab's memory. Reloading requires sign-in.
// This limits persistence; it does not make a running page immune to XSS.
const LEGACY_KEY = 'nexoradb_admin_access_token'
let token: string | null = null
let expiryTimer: number | undefined
const listeners = new Set<() => void>()
try {
  window.localStorage.removeItem(LEGACY_KEY)
  window.sessionStorage.removeItem(LEGACY_KEY)
} catch { /* Browser storage may be disabled. Authentication does not require it. */ }

export function readStoredToken() { return token }
export function subscribeToken(listener: () => void) {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}
export function persistToken(value: string, expiresIn: number) {
  window.clearTimeout(expiryTimer)
  token = value
  expiryTimer = window.setTimeout(clearStoredToken, Math.max(0, Math.min(expiresIn * 1000, 2_147_483_647)))
  listeners.forEach((listener) => listener())
}
export function clearStoredToken() {
  window.clearTimeout(expiryTimer)
  token = null
  listeners.forEach((listener) => listener())
}
