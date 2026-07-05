const TOKEN_STORAGE_KEY = 'nexoradb_admin_access_token'

export function readStoredToken() {
  return window.localStorage.getItem(TOKEN_STORAGE_KEY)
}

export function persistToken(accessToken: string) {
  window.localStorage.setItem(TOKEN_STORAGE_KEY, accessToken)
}

export function clearStoredToken() {
  window.localStorage.removeItem(TOKEN_STORAGE_KEY)
}
