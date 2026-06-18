import { API_BASE_URL } from './apiConfig'
import { ApiError } from './apiError'

type HttpMethod = 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE'

interface RequestOptions<TBody> {
  body?: TBody
  headers?: HeadersInit
  signal?: AbortSignal
}

async function request<TResponse, TBody = unknown>(
  method: HttpMethod,
  path: string,
  options: RequestOptions<TBody> = {},
): Promise<TResponse> {
  const response = await fetch(`${API_BASE_URL}${path}`, {
    method,
    headers: {
      'Content-Type': 'application/json',
      ...options.headers,
    },
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
    signal: options.signal,
  })

  const isJson = response.headers
    .get('content-type')
    ?.toLowerCase()
    .includes('application/json')

  const payload = isJson ? await response.json() : null

  if (!response.ok) {
    const message =
      payload && typeof payload.message === 'string'
        ? payload.message
        : `Request failed with status ${response.status}`

    throw new ApiError(message, response.status)
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
