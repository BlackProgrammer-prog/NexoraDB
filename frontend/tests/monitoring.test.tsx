import { Profiler, type PropsWithChildren } from 'react'
import { act, renderHook } from '@testing-library/react'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { io } from 'socket.io-client'
import { useMonitoringSocket } from '../src/features/monitoring/hooks/useMonitoringSocket'

vi.mock('socket.io-client', () => ({ io: vi.fn() }))

function createSocket() {
  const handlers = new Map<string, (payload?: unknown) => void>()
  const socket = {
    connected: true,
    handlers,
    on: vi.fn((event: string, handler: (payload?: unknown) => void) => { handlers.set(event, handler); return socket }),
    emit: vi.fn(),
    removeAllListeners: vi.fn(() => handlers.clear()),
    disconnect: vi.fn(),
  }
  return socket
}

let sockets: ReturnType<typeof createSocket>[]
let hidden = false
beforeEach(() => {
  vi.useFakeTimers()
  sockets = []
  hidden = false
  vi.spyOn(document, 'hidden', 'get').mockImplementation(() => hidden)
  vi.mocked(io).mockImplementation(() => {
    const socket = createSocket()
    sockets.push(socket)
    return socket as unknown as ReturnType<typeof io>
  })
})

describe('live monitoring render budget', () => {
  it('does not connect or expose metrics when signed out', () => {
    const { result } = renderHook(() => useMonitoringSocket('', null))
    expect(io).not.toHaveBeenCalled()
    expect(result.current).toMatchObject({ status: 'disconnected', samples: [] })
    expect(result.current.error).toContain('Sign in again')
  })

  it('coalesces a burst into one render commit and keeps the latest value', () => {
    let commits = 0
    function Wrapper({ children }: PropsWithChildren) {
      return <Profiler id="monitoring" onRender={() => { commits++ }}>{children}</Profiler>
    }
    const { result } = renderHook(() => useMonitoringSocket('', 'token'), { wrapper: Wrapper })
    const initial = commits
    act(() => {
      for (let i = 0; i < 500; i++) sockets[0].handlers.get('metrics')?.({ requestsPerSecond: i })
    })
    expect(commits).toBe(initial)
    act(() => vi.advanceTimersByTime(250))
    expect(commits - initial).toBe(1)
    expect(result.current.metrics.requestsPerSecond).toBe(499)
    expect(result.current.samples).toHaveLength(1)
  })

  it('keeps only the newest 30 chart samples', () => {
    const { result } = renderHook(() => useMonitoringSocket('', 'token'))
    for (let i = 0; i < 40; i++) {
      act(() => {
        sockets[0].handlers.get('metrics')?.({ requestsPerSecond: i })
        vi.advanceTimersByTime(250)
      })
    }
    expect(result.current.samples).toHaveLength(30)
    expect(result.current.samples[0].value).toBe(10)
    expect(result.current.samples.at(-1)?.value).toBe(39)
  })

  it('pauses hidden-tab paints and heartbeats, then resumes with the latest metrics', () => {
    const { result } = renderHook(() => useMonitoringSocket('', 'token'))
    hidden = true
    act(() => {
      sockets[0].handlers.get('metrics')?.({ requestsPerSecond: 7 })
      vi.advanceTimersByTime(5000)
    })
    expect(result.current.samples).toHaveLength(0)
    expect(sockets[0].emit).not.toHaveBeenCalled()
    hidden = false
    act(() => { document.dispatchEvent(new Event('visibilitychange')) })
    expect(result.current.metrics.requestsPerSecond).toBe(7)
    expect(sockets[0].emit).toHaveBeenCalledWith('metrics:refresh')
  })

  it.each([10_000, -5, Number.POSITIVE_INFINITY])('bounds numeric connection counts: %s', (count) => {
    const { result } = renderHook(() => useMonitoringSocket('', 'token'))
    act(() => {
      sockets[0].handlers.get('metrics')?.({ activeConnections: count })
      vi.advanceTimersByTime(250)
    })
    expect(result.current.metrics.activeConnections.length).toBe(count === 10_000 ? 500 : 0)
  })

  it('bounds connection arrays as well as numeric counts', () => {
    const { result } = renderHook(() => useMonitoringSocket('', 'token'))
    act(() => {
      sockets[0].handlers.get('metrics')?.({ activeConnections: Array.from({ length: 1000 }, (_, i) => String(i)) })
      vi.advanceTimersByTime(250)
    })
    expect(result.current.metrics.activeConnections).toHaveLength(500)
  })

  it('does not crash on a finite but invalid timestamp', () => {
    const { result } = renderHook(() => useMonitoringSocket('', 'token'))
    act(() => {
      sockets[0].handlers.get('metrics')?.({ requestsPerSecond: 2, receivedAt: 1e30 })
      vi.advanceTimersByTime(250)
    })
    expect(result.current.samples).toHaveLength(1)
    expect(result.current.metrics.requestsPerSecond).toBe(2)
  })

  it('clears private metrics and pending work when the token changes', () => {
    const { result, rerender } = renderHook(({ token }) => useMonitoringSocket('', token), { initialProps: { token: 'old' } })
    act(() => {
      sockets[0].handlers.get('metrics')?.({ requestsPerSecond: 123 })
      vi.advanceTimersByTime(250)
      sockets[0].handlers.get('metrics')?.({ requestsPerSecond: 456 })
    })
    rerender({ token: 'new' })
    expect(sockets[0].disconnect).toHaveBeenCalledTimes(1)
    expect(sockets[0].removeAllListeners).toHaveBeenCalledTimes(1)
    act(() => vi.advanceTimersByTime(250))
    expect(result.current.metrics.requestsPerSecond).toBe(0)
    expect(result.current.samples).toHaveLength(0)
    expect(result.current.status).toBe('connecting')
  })

  it('removes all listeners and timers on StrictMode teardown', () => {
    const { unmount } = renderHook(() => useMonitoringSocket('', 'token'), { reactStrictMode: true })
    expect(sockets).toHaveLength(2)
    unmount()
    expect(sockets.every((socket) => socket.disconnect.mock.calls.length === 1)).toBe(true)
    expect(sockets.every((socket) => socket.handlers.size === 0)).toBe(true)
    expect(vi.getTimerCount()).toBe(0)
  })
})
