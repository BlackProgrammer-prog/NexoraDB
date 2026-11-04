import { act, renderHook } from '@testing-library/react'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useQueryRunner } from '../src/features/query/hooks/useQueryRunner'
import { useQueryDiagnostics } from '../src/features/query/hooks/useQueryDiagnostics'
import { queryApi } from '../src/features/query/services/queryApi'
import type { CursorPage, QueryValidation } from '../src/features/query/types/query.types'
import { deferred } from './helpers'

vi.mock('../src/features/query/services/queryApi', () => ({ queryApi: {
  validate: vi.fn(), streamQuery: vi.fn(), executeQuery: vi.fn(), openCursor: vi.fn(), nextPage: vi.fn(), closeCursor: vi.fn(),
} }))
const page: CursorPage = { cursorId: 'lease', rows: [{ x: 1 }], columns: ['x'], hasMore: true, sequence: 1, totalRows: 1 }
beforeEach(() => { vi.mocked(queryApi.closeCursor).mockResolvedValue(null) })

describe('query workspace lifecycle', () => {
  it('keeps the write request key available after a failed open without retrying', async () => {
    vi.mocked(queryApi.openCursor).mockRejectedValue(new Error('connection lost'))
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('CREATE COLLECTION items;', 'cursor') })
    expect(result.current.requestKey).toMatch(/^[a-f0-9-]{36}$/)
    expect(queryApi.openCursor).toHaveBeenCalledWith({ query: 'CREATE COLLECTION items;', requestKey: result.current.requestKey }, expect.any(AbortSignal))
    expect(queryApi.openCursor).toHaveBeenCalledTimes(1)
    expect(result.current.state).toBe('error')
  })
  it('shows prepared statement failures and closes the cursor', async () => {
    vi.mocked(queryApi.openCursor).mockResolvedValue({ ...page, statementErrors: ['duplicate collection'] })
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('CREATE COLLECTION items;', 'cursor') })
    expect(result.current.state).toBe('error')
    expect(result.current.error?.message).toContain('earlier writes may have committed')
    expect(queryApi.closeCursor).toHaveBeenCalledWith('lease')
    expect(result.current.result?.statementErrors).toEqual(['duplicate collection'])
  })
  it('keeps only the newest 1,000 streaming rows while counting all received rows', async () => {
    vi.mocked(queryApi.streamQuery).mockImplementation(async (_query, _signal, receive) => {
      await receive(Array.from({ length: 1200 }, (_, x) => ({ x })))
      return 1200
    })
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM items', 'stream') })
    expect(result.current.result?.rows).toHaveLength(1000)
    expect(result.current.result?.rows[0]).toEqual({ x: 200 })
    expect(result.current.result).toMatchObject({ totalRows: 1200, previewTruncated: true, raw: null })
    expect(result.current.state).toBe('success')
  })
  it('marks partial results as failed when a stream has no completion marker', async () => {
    vi.mocked(queryApi.streamQuery).mockImplementation(async (_query, _signal, receive) => {
      await receive([{ x: 1 }])
      throw new Error('results are incomplete')
    })
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM items', 'stream') })
    expect(result.current.state).toBe('error')
    expect(result.current.result?.rows).toEqual([{ x: 1 }])
    expect(result.current.error?.message).toContain('incomplete')
  })
  it('replaces cursor pages and closes an idle cursor on unmount', async () => {
    vi.mocked(queryApi.openCursor).mockResolvedValue(page)
    vi.mocked(queryApi.nextPage).mockResolvedValue({ ...page, sequence: 2, rows: [{ x: 2 }], totalRows: 2 })
    const { result, unmount } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM items', 'cursor') })
    await act(async () => { await result.current.loadMore() })
    expect(queryApi.nextPage).toHaveBeenCalledWith('lease', 2, expect.any(AbortSignal))
    expect(result.current.result?.rows).toEqual([{ x: 2 }])
    unmount()
    expect(queryApi.closeCursor).toHaveBeenCalledWith('lease')
  })
  it('closes the server cursor when stopped without retrying pages', async () => {
    vi.mocked(queryApi.openCursor).mockResolvedValue(page)
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM items', 'cursor') })
    act(() => result.current.stop())
    expect(queryApi.closeCursor).toHaveBeenCalledWith('lease')
    expect(result.current.state).toBe('cancelled')
    expect(result.current.hasMore).toBe(false)
    expect(queryApi.nextPage).not.toHaveBeenCalled()
  })
  it('closes a cursor that arrives after the user has already stopped', async () => {
    const pending = deferred<CursorPage>()
    vi.mocked(queryApi.openCursor).mockReturnValue(pending.promise)
    const { result } = renderHook(useQueryRunner)
    let run!: Promise<void>
    act(() => { run = result.current.runQuery('SELECT * FROM items', 'cursor') })
    act(() => result.current.stop())
    await act(async () => { pending.resolve(page); await run })
    expect(queryApi.closeCursor).toHaveBeenCalledWith('lease')
    expect(result.current.result).toBeNull()
  })
  it('does not send two next-page requests for rapid repeated clicks', async () => {
    vi.mocked(queryApi.openCursor).mockResolvedValue(page)
    const pending = deferred<CursorPage>()
    vi.mocked(queryApi.nextPage).mockReturnValue(pending.promise)
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM items', 'cursor') })
    let next!: Promise<void>
    act(() => { next = result.current.loadMore(); void result.current.loadMore() })
    expect(queryApi.nextPage).toHaveBeenCalledTimes(1)
    await act(async () => { pending.resolve({ ...page, sequence: 2 }); await next })
  })
  it('exposes an empty successfully completed stream as a result', async () => {
    vi.mocked(queryApi.streamQuery).mockResolvedValue(0)
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SELECT * FROM empty', 'stream') })
    expect(result.current.result?.rows).toEqual([])
    expect(result.current.state).toBe('success')
  })
})

describe('debounced parser diagnostics', () => {
  it('aborts obsolete validation requests and ignores late responses', async () => {
    vi.useFakeTimers()
    const old = deferred<QueryValidation>()
    const fresh: QueryValidation = { valid: true, diagnostics: [], statementCount: 1, cursorSupported: true }
    vi.mocked(queryApi.validate).mockReturnValueOnce(old.promise).mockResolvedValueOnce(fresh)
    const { result, rerender, unmount } = renderHook(({ query }) => useQueryDiagnostics(query), { initialProps: { query: 'old' } })
    await act(async () => { await vi.advanceTimersByTimeAsync(400) })
    rerender({ query: 'new' })
    expect(vi.mocked(queryApi.validate).mock.calls[0][1]?.aborted).toBe(true)
    await act(async () => { await vi.advanceTimersByTimeAsync(400) })
    await act(async () => { old.resolve({ ...fresh, valid: false }) })
    expect(result.current.response?.valid).toBe(true)
    unmount()
  })
})
