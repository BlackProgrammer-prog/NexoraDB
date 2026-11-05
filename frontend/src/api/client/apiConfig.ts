export function secureEndpoint(configured: string, fallback: string): string {
  const value = configured || fallback
  const url = new URL(value || window.location.origin, window.location.origin)
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash) {
    throw new Error('API endpoint must be an HTTP(S) URL without credentials, query or fragment')
  }
  const loopback = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)
  if (url.protocol === 'http:' && (!loopback || window.location.protocol === 'https:')) {
    throw new Error('Remote API endpoints require HTTPS')
  }
  return value ? url.href.replace(/\/$/, '') : ''
}

export const API_BASE_URL = secureEndpoint(
  import.meta.env.VITE_API_BASE_URL ?? '',
  import.meta.env.DEV ? 'http://localhost:8000' : '',
)
export const USE_MOCK_API = import.meta.env.DEV && import.meta.env.VITE_USE_MOCK_API === 'true'
