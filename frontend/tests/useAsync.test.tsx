import { act, renderHook, waitFor } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { useAsync } from '../src/shared/hooks/useAsync'
import { deferred } from './helpers'

describe('request ownership in development', () => {
  it('survives StrictMode effect setup/cleanup with only the live request committing', async () => {
    const requests: Array<{ signal: AbortSignal; task: ReturnType<typeof deferred<string>> }> = []
    const loader = vi.fn((signal: AbortSignal) => {
      const task = deferred<string>()
      requests.push({ signal, task })
      return task.promise
    })
    const { result } = renderHook(() => useAsync(loader), { reactStrictMode: true })
    expect(requests).toHaveLength(2)
    expect(requests[0].signal.aborted).toBe(true)
    await act(async () => { requests[1].task.resolve('current') })
    await act(async () => { requests[0].task.resolve('stale') })
    expect(result.current.data).toBe('current')
    expect(result.current.isLoading).toBe(false)
  })

  it('ignores an old response after the resource changes, even if transport ignores abort', async () => {
    const old = deferred<string>()
    const next = deferred<string>()
    const oldLoader = vi.fn(() => old.promise)
    const newLoader = vi.fn(() => next.promise)
    const { result, rerender } = renderHook(({ loader }) => useAsync(loader), { initialProps: { loader: oldLoader } })
    rerender({ loader: newLoader })
    expect(result.current.data).toBeNull()
    expect(result.current.isLoading).toBe(true)
    await act(async () => { next.resolve('new') })
    await act(async () => { old.resolve('old') })
    expect(result.current.data).toBe('new')
  })

  it('lets the latest refetch win when requests complete in reverse order', async () => {
    const first = deferred<string>()
    const second = deferred<string>()
    const loader = vi.fn().mockReturnValueOnce(first.promise).mockReturnValueOnce(second.promise)
    const { result } = renderHook(() => useAsync<string>(loader))
    let refresh!: Promise<void>
    act(() => { refresh = result.current.refetch() })
    await act(async () => { second.resolve('second'); await refresh })
    await act(async () => { first.resolve('first') })
    expect(result.current.data).toBe('second')
  })

  it('does not let a callback from the previous resource restart its fetch', () => {
    const first = vi.fn(() => new Promise<string>(() => {}))
    const second = vi.fn(() => new Promise<string>(() => {}))
    const { result, rerender } = renderHook(({ loader }) => useAsync(loader), { initialProps: { loader: first } })
    const staleRefetch = result.current.refetch
    rerender({ loader: second })
    act(() => { void staleRefetch() })
    expect(first).toHaveBeenCalledTimes(1)
    expect(second).toHaveBeenCalledTimes(1)
  })

  it('aborts on unmount and ignores a retained refetch callback', async () => {
    let signal!: AbortSignal
    const loader = vi.fn((input: AbortSignal) => { signal = input; return new Promise<string>(() => {}) })
    const { result, unmount } = renderHook(() => useAsync(loader))
    const refresh = result.current.refetch
    unmount()
    expect(signal.aborted).toBe(true)
    await refresh()
    expect(loader).toHaveBeenCalledTimes(1)
  })

  it('exposes rejected requests and can recover on refetch', async () => {
    const loader = vi.fn().mockRejectedValueOnce(new Error('offline')).mockResolvedValueOnce('recovered')
    const { result } = renderHook(() => useAsync<string>(loader))
    await waitFor(() => expect(result.current.error?.message).toBe('offline'))
    await act(async () => { await result.current.refetch() })
    expect(result.current).toMatchObject({ data: 'recovered', error: null, isLoading: false })
  })

  it('converts a synchronous loader exception into an error state', async () => {
    const loader = vi.fn((): Promise<string> => { throw new Error('configuration failed') })
    const { result } = renderHook(() => useAsync(loader))
    await waitFor(() => expect(result.current.error?.message).toBe('configuration failed'))
    expect(result.current.isLoading).toBe(false)
  })
})
