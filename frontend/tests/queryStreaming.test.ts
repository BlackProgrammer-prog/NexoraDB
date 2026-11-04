import { describe, expect, it, vi } from 'vitest'
import { readQueryStream, type Row } from '../src/features/query/services/queryStream'
import { editorDiagnostic } from '../src/features/query/editor/diagnostics'

function response(text: string, chunkSize = 7) {
  const bytes = new TextEncoder().encode(text)
  let from = 0
  return new Response(new ReadableStream<Uint8Array>({
    pull(controller) {
      if (from === bytes.length) controller.close()
      else { const to = Math.min(bytes.length, from + chunkSize); controller.enqueue(bytes.slice(from, to)); from = to }
    },
  }))
}
const signal = () => new AbortController().signal

describe('bounded NDJSON query transport', () => {
  it('preserves statement envelopes and receipts without interpreting user fields as execution status', async () => {
    const documents = [{ success: false }, { statement: 1, statementType: 'Select', row: { success: false } },
      { statement: 1, receipt: true, success: true, rowCount: 1 }]
    const text = documents.map((document) => JSON.stringify({ document })).join('\n')+'\n{"end":true}\n'
    const rows: Row[] = []
    expect(await readQueryStream(response(text), (batch) => { rows.push(...batch) }, signal())).toBe(3)
    expect(rows).toEqual(documents)
  })
  it('handles split UTF-8 characters and frames, not only network-aligned rows', async () => {
    const rows: Row[] = []
    const result = await readQueryStream(response('{"document":{"name":"دیتابیس 😀"}}\n{"end":true}\n', 1), (batch) => { rows.push(...batch) }, signal())
    expect(result).toBe(1)
    expect(rows).toEqual([{ name: 'دیتابیس 😀' }])
  })
  it('processes large network chunks with bounded callback batches', async () => {
    const text = Array.from({ length: 10_000 }, (_, i) => JSON.stringify({ document: { i } })).join('\n') + '\n{"end":true}\n'
    const sizes: number[] = []
    expect(await readQueryStream(response(text, text.length), (batch) => { sizes.push(batch.length) }, signal())).toBe(10_000)
    expect(Math.max(...sizes)).toBeLessThanOrEqual(128)
    expect(sizes.reduce((total, size) => total + size, 0)).toBe(10_000)
  })
  it('accepts an empty completed stream', async () => {
    const receive = vi.fn()
    expect(await readQueryStream(response('{"end":true}\n'), receive, signal())).toBe(0)
    expect(receive).not.toHaveBeenCalled()
  })
  it('retains received script receipts when a failure shares their network chunk', async () => {
    const text = '{"document":{"statement":1,"receipt":true,"success":true}}\n{"error":"statement 2 failed"}\n'
    const received: Row[] = []
    await expect(readQueryStream(response(text, text.length), (batch) => { received.push(...batch) }, signal())).rejects.toThrow('statement 2 failed')
    expect(received).toEqual([{ statement: 1, receipt: true, success: true }])
  })
  it.each([
    ['{"document":{"x":1}}\n', 'incomplete'],
    ['{"error":"cursor revoked"}\n', 'cursor revoked'],
    ['{"end":true}\n{"document":{"x":1}}\n', 'after stream completion'],
    ['{"document":[]}\n{"end":true}\n', 'Invalid stream row'],
    ['{bad}\n', 'JSON'],
  ])('rejects a failed or invalid stream: %s', async (text, message) => {
    await expect(readQueryStream(response(text), vi.fn(), signal())).rejects.toThrow(message)
  })
  it('rejects an oversized unfinished record', async () => {
    await expect(readQueryStream(response('x'.repeat(2_097_153), 65_536), vi.fn(), signal())).rejects.toThrow('2 MiB')
  })
  it('cancels a stalled reader immediately when the caller stops', async () => {
    const cancel = vi.fn()
    const controller = new AbortController()
    const rejected = expect(readQueryStream(new Response(new ReadableStream({ cancel })), vi.fn(), controller.signal)).rejects.toHaveProperty('name', 'AbortError')
    controller.abort()
    await rejected
    expect(cancel).toHaveBeenCalled()
  })
  it('waits for each consumer batch before reading more', async () => {
    const receive = vi.fn(async () => { await Promise.resolve() })
    await readQueryStream(response('{"document":{"x":1}}\n{"end":true}\n', 20), receive, signal())
    expect(receive).toHaveBeenCalledTimes(1)
  })
})

describe('editor diagnostic coordinates', () => {
  it('maps Python Unicode columns to CodeMirror UTF-16 offsets', () => {
    const query = "-- 😀\nSELECT '😀' FRUM items;"
    const marker = editorDiagnostic(query, { message: 'Expected FROM', line: 2, column: 12 })
    expect(query.slice(marker.from, marker.to)).toBe('F')
  })
  it('keeps EOF markers and invalid backend positions inside the document', () => {
    const marker = editorDiagnostic('SELECT', { message: 'Expected FROM', line: 100, column: 100 })
    expect(marker.from).toBe(6)
    expect(marker.to).toBe(6)
  })
})
