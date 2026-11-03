import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { httpClient } from '../src/api/client/httpClient'
import { secureEndpoint } from '../src/api/client/apiConfig'
import { clearStoredToken, persistToken, readStoredToken } from '../src/features/auth/services/authTokenStorage'
import { deferred } from './helpers'

const fetchMock = vi.fn<typeof fetch>()
const json = (value: unknown, status = 200) => new Response(JSON.stringify(value), {
  status, headers: { 'content-type': 'application/json' },
})

beforeEach(() => { clearStoredToken(); fetchMock.mockReset(); vi.stubGlobal('fetch', fetchMock) })
afterEach(() => clearStoredToken())

describe('console HTTP transport', () => {
  it('sends a bearer header without cookies, redirects, caches or referrers', async () => {
    persistToken('private', 60)
    fetchMock.mockResolvedValue(json({ ok: true }))
    await expect(httpClient.get('/collections')).resolves.toEqual({ ok: true })
    const [url, options] = fetchMock.mock.calls[0]
    expect(String(url)).toMatch(/\/collections$/)
    expect(options).toMatchObject({ cache: 'no-store', credentials: 'omit', redirect: 'error', referrerPolicy: 'no-referrer',
      headers: { Authorization: 'Bearer private' } })
    expect(String(url)).not.toContain('private')
  })

  it.each(['//other.example/steal', 'https://other.example/steal', 'relative/path'])('rejects a non-local API path: %s', async (path) => {
    await expect(httpClient.get(path)).rejects.toThrow('Invalid API path')
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('accepts a bodyless successful delete response', async () => {
    fetchMock.mockResolvedValue(new Response(null, { status: 204 }))
    await expect(httpClient.delete('/documents/one')).resolves.toBeNull()
  })

  it.each([
    [{ message: 'denied' }, 'denied'],
    [{ detail: 'invalid query' }, 'invalid query'],
    [{ detail: [{ msg: 'missing field' }] }, 'missing field'],
  ])('exposes a structured API error without retrying the request', async (payload, message) => {
    fetchMock.mockResolvedValue(json(payload, 403))
    await expect(httpClient.post('/query/execute', { query: 'test' })).rejects.toThrow(String(message))
    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('clears the current token on 401', async () => {
    persistToken('current', 60)
    fetchMock.mockResolvedValue(json({ message: 'expired' }, 401))
    await expect(httpClient.get('/auth/me')).rejects.toThrow('expired')
    expect(readStoredToken()).toBeNull()
  })

  it('does not let an old request invalidate a newer session', async () => {
    persistToken('old', 60)
    const response = deferred<Response>()
    fetchMock.mockReturnValue(response.promise)
    const pending = httpClient.get('/auth/me')
    const rejected = expect(pending).rejects.toThrow('expired')
    persistToken('new', 60)
    response.resolve(json({ message: 'expired' }, 401))
    await rejected
    expect(readStoredToken()).toBe('new')
  })

  it('rejects successful HTML instead of treating it as API data', async () => {
    fetchMock.mockResolvedValue(new Response('<script>bad()</script>', { headers: { 'content-type': 'text/html' } }))
    await expect(httpClient.get('/collections')).rejects.toThrow('non-JSON')
  })

  it('rejects malformed JSON', async () => {
    fetchMock.mockResolvedValue(new Response('{', { headers: { 'content-type': 'application/json' } }))
    await expect(httpClient.get('/collections')).rejects.toBeInstanceOf(SyntaxError)
  })

  it('counts request size in UTF-8 bytes, not character count', async () => {
    await expect(httpClient.post('/query/execute', { query: 'ی'.repeat(600_000) })).rejects.toThrow('1 MiB')
    expect(fetchMock).not.toHaveBeenCalled()
  })

  it('parses JSON split across tiny network chunks and split Unicode bytes', async () => {
    const bytes = new TextEncoder().encode(JSON.stringify({ text: 'دیتابیس' }))
    let cursor = 0
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) {
        if (cursor < bytes.length) controller.enqueue(bytes.slice(cursor, ++cursor))
        else controller.close()
      },
    })
    fetchMock.mockResolvedValue(new Response(stream, { headers: { 'content-type': 'application/json' } }))
    await expect(httpClient.get('/unicode')).resolves.toEqual({ text: 'دیتابیس' })
  })

  it('cancels the response stream when the 8 MiB budget is exceeded', async () => {
    const cancel = vi.fn()
    const stream = new ReadableStream<Uint8Array>({
      pull(controller) { controller.enqueue(new Uint8Array(65_536)) },
      cancel,
    })
    fetchMock.mockResolvedValue(new Response(stream, { headers: { 'content-type': 'application/json' } }))
    await expect(httpClient.get('/too-large')).rejects.toThrow('8 MiB')
    expect(cancel).toHaveBeenCalledTimes(1)
  })

  it('propagates caller cancellation and releases its timeout', async () => {
    vi.useFakeTimers()
    fetchMock.mockImplementation((_url, options) => new Promise((_resolve, reject) => {
      options?.signal?.addEventListener('abort', () => reject(options.signal?.reason), { once: true })
    }))
    const controller = new AbortController()
    const rejected = expect(httpClient.get('/pending', { signal: controller.signal })).rejects.toHaveProperty('name', 'AbortError')
    controller.abort()
    await rejected
    expect(vi.getTimerCount()).toBe(0)
  })

  it('times out a stalled request without an automatic retry', async () => {
    vi.useFakeTimers()
    fetchMock.mockImplementation((_url, options) => new Promise((_resolve, reject) => {
      options?.signal?.addEventListener('abort', () => reject(options.signal?.reason), { once: true })
    }))
    const rejected = expect(httpClient.get('/pending')).rejects.toHaveProperty('name', 'TimeoutError')
    await vi.advanceTimersByTimeAsync(35_000)
    await rejected
    expect(fetchMock).toHaveBeenCalledTimes(1)
    expect(vi.getTimerCount()).toBe(0)
  })
})

describe('endpoint configuration', () => {
  it.each(['javascript:alert(1)', 'https://user:pass@api.example.test', 'https://api.example.test/?token=secret',
    'https://api.example.test/#secret', 'http://api.example.test'])('rejects an unsafe endpoint: %s', (url) => {
    expect(() => secureEndpoint(url, '')).toThrow()
  })
  it('allows a configured HTTPS API and the same-origin default', () => {
    expect(secureEndpoint('https://api.example.test/', '')).toBe('https://api.example.test')
    expect(secureEndpoint('', '')).toBe('')
  })
})
