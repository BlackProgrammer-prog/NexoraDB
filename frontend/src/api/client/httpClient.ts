import { API_BASE_URL } from './apiConfig'
import { ApiError } from './apiError'
import { clearStoredToken, readStoredToken } from '../../features/auth/services/authTokenStorage'

type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE'

interface RequestOptions<TBody> {
  body?: TBody
  headers?: HeadersInit
  signal?: AbortSignal
}

function getErrorMessage(payload: unknown, statusCode: number) {
  if (payload && typeof payload === 'object' && 'message' in payload) {
    const message = (payload as { message?: unknown }).message

    if (typeof message === 'string') {
      return message
    }
  }

  if (payload && typeof payload === 'object' && 'detail' in payload) {
    const detail = (payload as { detail?: unknown }).detail

    if (typeof detail === 'string') {
      return detail
    }

    if (Array.isArray(detail)) {
      const firstMessage = detail.find((item): item is { msg: string } => {
        if (!item || typeof item !== 'object' || !('msg' in item)) {
          return false
        }

        return typeof (item as { msg?: unknown }).msg === 'string'
      })

      if (firstMessage) {
        return firstMessage.msg
      }
    }
  }

  return `Request failed with status ${statusCode}`
}

async function request<TResponse, TBody = unknown>(
  method: HttpMethod,
  path: string,
  options: RequestOptions<TBody> = {},
): Promise<TResponse> {
  if (!path.startsWith('/') || path.startsWith('//')) throw new Error('Invalid API path')
  const accessToken = readStoredToken()
  const controller = new AbortController()
  const abort = () => controller.abort(options.signal?.reason)
  options.signal?.addEventListener('abort', abort, { once: true })
  if (options.signal?.aborted) abort()
  const timeout = window.setTimeout(() => controller.abort(new DOMException('Request timed out', 'TimeoutError')), 35_000)
  try {
    const body = options.body === undefined ? undefined : JSON.stringify(options.body)
    if (body && new TextEncoder().encode(body).length > 1_048_576) {
      throw new Error('Request exceeds the console 1 MiB limit. Use the streaming API for bulk data.')
    }
    const response = await fetch(`${API_BASE_URL}${path}`, {
      method,
      cache: 'no-store',
      credentials: 'omit',
      redirect: 'error',
      referrerPolicy: 'no-referrer',
      headers: {
        'Content-Type': 'application/json',
        ...(accessToken ? { Authorization: `Bearer ${accessToken}` } : {}),
        ...options.headers,
      },
      body,
      signal: controller.signal,
    })
    if (response.status === 401 && accessToken && readStoredToken() === accessToken) clearStoredToken()
    const isJson = response.headers.get('content-type')?.toLowerCase().includes('application/json')
    let payload: unknown = null
    if (isJson && response.status !== 204 && response.status !== 205) {
      const reader = response.body?.getReader()
      if (reader) {
        const limit = 8_388_608
        let buffer: Uint8Array = new Uint8Array(65_536)
        let bytes = 0
        try {
          for (;;) {
            const { done, value } = await reader.read()
            if (done) break
            const nextBytes = bytes + value.byteLength
            if (nextBytes > limit) {
              await reader.cancel()
              throw new Error('Response exceeds the console 8 MiB limit. Narrow the query or use the paginated API.')
            }
            if (nextBytes > buffer.byteLength) {
              const expanded = new Uint8Array(Math.min(limit, Math.max(nextBytes, buffer.byteLength * 2)))
              expanded.set(buffer.subarray(0, bytes))
              buffer = expanded
            }
            buffer.set(value, bytes)
            bytes = nextBytes
          }
        } finally { reader.releaseLock() }
        payload = bytes ? JSON.parse(new TextDecoder().decode(buffer.subarray(0, bytes))) : null
      }
    } else if (!response.ok || response.body) {
      await response.body?.cancel()
    }
    if (!response.ok) throw new ApiError(getErrorMessage(payload, response.status), response.status)
    if (!isJson && response.status !== 204 && response.status !== 205) throw new Error('API returned a non-JSON response')
    return payload as TResponse
  } finally {
    window.clearTimeout(timeout)
    options.signal?.removeEventListener('abort', abort)
  }
}

export const httpClient = {
  get: <TResponse>(path: string, options?: RequestOptions<never>) =>
    request<TResponse>('GET', path, options),
  post: <TResponse, TBody = unknown>(
    path: string,
    body: TBody,
    options?: Omit<RequestOptions<TBody>, 'body'>,
  ) => request<TResponse, TBody>('POST', path, { ...options, body }),
  put: <TResponse, TBody = unknown>(
    path: string,
    body: TBody,
    options?: Omit<RequestOptions<TBody>, 'body'>,
  ) => request<TResponse, TBody>('PUT', path, { ...options, body }),
  patch: <TResponse, TBody = unknown>(
    path: string,
    body: TBody,
    options?: Omit<RequestOptions<TBody>, 'body'>,
  ) => request<TResponse, TBody>('PATCH', path, { ...options, body }),
  delete: <TResponse>(path: string, options?: RequestOptions<never>) =>
    request<TResponse>('DELETE', path, options),
}
