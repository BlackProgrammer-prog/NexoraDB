import { API_BASE_URL } from '../../../api/client/apiConfig'
import { ApiError } from '../../../api/client/apiError'
import { clearStoredToken, readStoredToken } from '../../auth/services/authTokenStorage'
import type { QueryRequest } from '../types/query.types'

export type Row = Record<string, unknown>
export async function readQueryStream(response: Response, onBatch: (rows: Row[]) => Promise<void> | void,
  signal: AbortSignal): Promise<number> {
  const reader = response.body?.getReader()
  if (!reader) throw new Error('Streaming response has no body')
  const abortRead = () => { void reader.cancel(signal.reason).catch(() => {}) }
  signal.addEventListener('abort', abortRead, { once: true })
  const decoder = new TextDecoder('utf-8', { fatal: true })
  let pending = '', ended = false, total = 0
  let batch: Row[] = []
  async function line(text: string) {
    if (!text.trim()) return
    if (ended) throw new Error('Unexpected data after stream completion')
    if (new TextEncoder().encode(text).length > 2_097_152) throw new Error('Stream record exceeds 2 MiB')
    const frame = JSON.parse(text)
    if (!frame || typeof frame !== 'object') throw new Error('Invalid stream frame')
    if (typeof frame.error === 'string') {
      if (batch.length) { await onBatch(batch); batch = [] }
      throw new Error(frame.error)
    }
    if (frame.end === true) {
      if ('document' in frame) throw new Error('Invalid stream completion frame')
      ended = true; return
    }
    if (!frame.document || typeof frame.document !== 'object' || Array.isArray(frame.document)) throw new Error('Invalid stream row')
    batch.push(frame.document)
    total++
    if (batch.length === 128) { await onBatch(batch); batch = [] }
  }
  async function text(value: string) {
    let from = 0
    for (;;) {
      const newline = value.indexOf('\n', from)
      const to = newline < 0 ? value.length : newline
      if (pending.length + to - from > 2_097_152) throw new Error('Stream record exceeds 2 MiB')
      pending += value.slice(from, to)
      if (newline < 0) return
      await line(pending)
      pending = ''
      from = newline + 1
    }
  }
  try {
    for (;;) {
      signal.throwIfAborted()
      const { done, value } = await reader.read()
      signal.throwIfAborted()
      if (done) break
      await text(decoder.decode(value, { stream: true }))
      if (batch.length) { await onBatch(batch); batch = [] }
    }
    await text(decoder.decode())
    if (pending.trim()) await line(pending)
    if (!ended) throw new Error('Stream ended without completion; results are incomplete')
    if (batch.length) await onBatch(batch)
    return total
  } finally {
    signal.removeEventListener('abort', abortRead)
    await reader.cancel().catch(() => {})
    reader.releaseLock()
  }
}

export async function streamQuery(input: QueryRequest, signal: AbortSignal, onBatch: (rows: Row[]) => Promise<void> | void) {
  const token = readStoredToken()
  const controller = new AbortController()
  const abort = () => controller.abort(signal.reason)
  signal.addEventListener('abort', abort, { once: true })
  if (signal.aborted) abort()
  const timeout = window.setTimeout(() => controller.abort(new DOMException('Stream deadline exceeded', 'TimeoutError')), 120_000)
  try {
    const response = await fetch(`${API_BASE_URL}/query/stream`, { method: 'POST', credentials: 'omit', cache: 'no-store',
      redirect: 'error', referrerPolicy: 'no-referrer', signal: controller.signal,
      headers: { 'Content-Type': 'application/json', 'Idempotency-Key': input.requestKey ?? crypto.randomUUID(), ...(token ? { Authorization: `Bearer ${token}` } : {}) },
      body: JSON.stringify({ ...input, pageSize: 128 }) })
    if (response.status === 401 && readStoredToken() === token) clearStoredToken()
    if (!response.ok) {
      // Error bodies have the normal console budget; never buffer an unlimited error response.
      const reader = response.body?.getReader()
      const errorDecoder = new TextDecoder()
      let body = '', size = 0
      try {
        if (reader) for (;;) {
          const part = await reader.read()
          if (part.done) break
          size += part.value.byteLength
          if (size > 65_536) throw new Error('Streaming error response exceeds 64 KiB')
          body += errorDecoder.decode(part.value, { stream: true })
        }
        body += errorDecoder.decode()
      } finally { await reader?.cancel().catch(() => {}); reader?.releaseLock() }
      let details: { message?: string; detail?: string } = {}
      try { details = JSON.parse(body) } catch { /* Non-JSON gateway error. */ }
      throw new ApiError(details.message ?? (typeof details.detail === 'string' ? details.detail : `Stream failed (${response.status})`), response.status, details)
    }
    if (!response.headers.get('content-type')?.includes('application/x-ndjson')) {
      await response.body?.cancel()
      throw new Error('Server did not return an NDJSON stream')
    }
    return await readQueryStream(response, onBatch, controller.signal)
  } finally {
    window.clearTimeout(timeout)
    signal.removeEventListener('abort', abort)
  }
}
