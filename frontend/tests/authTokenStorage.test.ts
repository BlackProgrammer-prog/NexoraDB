import { beforeEach, describe, expect, it, vi } from 'vitest'

beforeEach(() => { vi.resetModules(); vi.useFakeTimers() })

describe('memory-only admin token lifecycle', () => {
  it('removes legacy browser tokens without restoring a session', async () => {
    localStorage.setItem('nexoradb_admin_access_token', 'old')
    sessionStorage.setItem('nexoradb_admin_access_token', 'old')
    const store = await import('../src/features/auth/services/authTokenStorage')
    expect(store.readStoredToken()).toBeNull()
    expect(localStorage.getItem('nexoradb_admin_access_token')).toBeNull()
    expect(sessionStorage.getItem('nexoradb_admin_access_token')).toBeNull()
  })

  it('stores a live token only in memory, not cookies or either browser storage', async () => {
    const store = await import('../src/features/auth/services/authTokenStorage')
    store.persistToken('private-token', 60)
    expect(store.readStoredToken()).toBe('private-token')
    expect(localStorage.length).toBe(0)
    expect(sessionStorage.length).toBe(0)
    expect(document.cookie).toBe('')
  })

  it('expires and notifies subscribers at the deadline', async () => {
    const store = await import('../src/features/auth/services/authTokenStorage')
    const listener = vi.fn()
    const unsubscribe = store.subscribeToken(listener)
    store.persistToken('token', 2)
    await vi.advanceTimersByTimeAsync(1999)
    expect(store.readStoredToken()).toBe('token')
    await vi.advanceTimersByTimeAsync(1)
    expect(store.readStoredToken()).toBeNull()
    expect(listener).toHaveBeenCalledTimes(2)
    unsubscribe()
  })

  it('does not let the previous expiry timer erase a newer token', async () => {
    const store = await import('../src/features/auth/services/authTokenStorage')
    store.persistToken('old', 1)
    await vi.advanceTimersByTimeAsync(500)
    store.persistToken('new', 10)
    await vi.advanceTimersByTimeAsync(1000)
    expect(store.readStoredToken()).toBe('new')
    store.clearStoredToken()
    expect(vi.getTimerCount()).toBe(0)
  })

  it('stops notifying an unsubscribed consumer', async () => {
    const store = await import('../src/features/auth/services/authTokenStorage')
    const listener = vi.fn()
    const unsubscribe = store.subscribeToken(listener)
    unsubscribe()
    store.persistToken('new', 60)
    expect(listener).not.toHaveBeenCalled()
  })

  it('starts signed out when the module is recreated, like a fresh page load', async () => {
    const first = await import('../src/features/auth/services/authTokenStorage')
    first.persistToken('token', 60)
    vi.resetModules()
    const fresh = await import('../src/features/auth/services/authTokenStorage')
    expect(fresh.readStoredToken()).toBeNull()
    first.clearStoredToken()
  })

  it('continues working when browser storage access is blocked', async () => {
    vi.spyOn(Storage.prototype, 'removeItem').mockImplementation(() => { throw new DOMException('blocked', 'SecurityError') })
    const store = await import('../src/features/auth/services/authTokenStorage')
    store.persistToken('memory-still-works', 60)
    expect(store.readStoredToken()).toBe('memory-still-works')
    store.clearStoredToken()
  })
})
