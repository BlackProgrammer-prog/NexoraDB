import { act, renderHook } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { useQueryRunner } from '../src/features/query/hooks/useQueryRunner'
import { queryApi } from '../src/features/query/services/queryApi'
import type { QueryResult } from '../src/features/query/types/query.types'
import { deferred } from './helpers'

vi.mock('../src/features/query/services/queryApi', () => ({ queryApi: { executeQuery: vi.fn() } }))
const response: QueryResult = { columns: ['value'], rows: [{ value: 'current' }], raw: {}, executionTimeMs: 1 }

describe('query runner request lifecycle', () => {
  it('does not report a partially failed script as successful just because HTTP returned 200', async () => {
    vi.mocked(queryApi.executeQuery).mockResolvedValue({ ...response, statementErrors: ['collection does not exist'] })
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('SHOW COLLECTIONS; SELECT * FROM missing;') })
    expect(result.current.state).toBe('error')
    expect(result.current.error?.message).toContain('Earlier writes may have committed')
    expect(result.current.result?.statementErrors).toEqual(['collection does not exist'])
  })
  it('starts idle and exposes a successful result', async () => {
    vi.mocked(queryApi.executeQuery).mockResolvedValue(response)
    const { result } = renderHook(useQueryRunner)
    expect(result.current.isIdle).toBe(true)
    await act(async () => { await result.current.runQuery('SELECT value FROM users') })
    expect(result.current).toMatchObject({ result: response, error: null, isLoading: false })
  })

  it('ignores a late response from a replaced query', async () => {
    const old = deferred<QueryResult>()
    vi.mocked(queryApi.executeQuery).mockReturnValueOnce(old.promise).mockResolvedValueOnce(response)
    const { result } = renderHook(useQueryRunner)
    let oldRun!: Promise<void>
    act(() => { oldRun = result.current.runQuery('old') })
    await act(async () => { await result.current.runQuery('new') })
    expect(vi.mocked(queryApi.executeQuery).mock.calls[0][1]?.aborted).toBe(true)
    await act(async () => { old.resolve({ ...response, rows: [{ value: 'stale' }] }); await oldRun })
    expect(result.current.result?.rows).toEqual(response.rows)
  })

  it('does not retain a previous result after a failed query', async () => {
    vi.mocked(queryApi.executeQuery).mockResolvedValueOnce(response).mockRejectedValueOnce(new Error('denied'))
    const { result } = renderHook(useQueryRunner)
    await act(async () => { await result.current.runQuery('success') })
    await act(async () => { await result.current.runQuery('fail') })
    expect(result.current.result).toBeNull()
    expect(result.current.error?.message).toBe('denied')
  })

  it('aborts the active query on unmount', () => {
    vi.mocked(queryApi.executeQuery).mockImplementation(() => new Promise(() => {}))
    const { result, unmount } = renderHook(useQueryRunner)
    act(() => { void result.current.runQuery('pending') })
    unmount()
    expect(vi.mocked(queryApi.executeQuery).mock.calls[0][1]?.aborted).toBe(true)
  })
})
