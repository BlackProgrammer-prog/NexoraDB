import { API_BASE_URL } from './apiConfig'
import { ApiError } from './apiError'
import { readStoredToken } from '../../features/auth/services/authTokenStorage'

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
  const accessToken = readStoredToken()
  const response = await fetch(`${API_BASE_URL}${path}`, {
    method,
    cache: method === 'GET' ? 'no-store' : undefined,
    headers: {
      'Content-Type': 'application/json',
      ...(accessToken ? { Authorization: `Bearer ${accessToken}` } : {}),
      ...options.headers,
    },
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
    signal: options.signal,
  })

  const isJson = response.headers
    .get('content-type')
    ?.toLowerCase()
    .includes('application/json')

  const hasNoContent = response.status === 204 || response.status === 205
  const payload = hasNoContent ? null : isJson ? await response.json() : null

  if (!response.ok) {
    throw new ApiError(getErrorMessage(payload, response.status), response.status)
  }

  return payload as TResponse
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
