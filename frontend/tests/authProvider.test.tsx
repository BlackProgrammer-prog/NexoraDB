import type { PropsWithChildren } from 'react'
import { act, renderHook } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { AuthProvider } from '../src/features/auth/context/AuthContext'
import { useAuth } from '../src/features/auth/context/useAuth'
import { authApi } from '../src/features/auth/services/authApi'
import { clearStoredToken, persistToken, readStoredToken } from '../src/features/auth/services/authTokenStorage'
import { admin, deferred } from './helpers'

vi.mock('../src/features/auth/services/authApi', () => ({ authApi: { login: vi.fn(), me: vi.fn(), register: vi.fn() } }))
const input = { username: 'root', password: 'test-only-password' }
function Wrapper({ children }: PropsWithChildren) {
  return <AuthProvider>{children}</AuthProvider>
}

beforeEach(() => {
  clearStoredToken()
  vi.mocked(authApi.login).mockResolvedValue({ accessToken: 'test-token', tokenType: 'bearer', expiresIn: 60, user: admin })
  vi.mocked(authApi.me).mockResolvedValue(admin)
})
afterEach(() => { clearStoredToken() })

describe('admin session integration under StrictMode', () => {
  it('revalidates an existing in-memory session across root StrictMode effect replay', async () => {
    persistToken('remounted-session', 60)
    const obsolete = deferred<typeof admin>()
    vi.mocked(authApi.me).mockReturnValueOnce(obsolete.promise).mockResolvedValueOnce(admin)
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    expect(vi.mocked(authApi.me)).toHaveBeenCalledTimes(2)
    expect(vi.mocked(authApi.me).mock.calls[0][1]?.aborted).toBe(true)
    await act(async () => { obsolete.resolve({ ...admin, displayName: 'Stale identity' }) })
    expect(result.current.user?.displayName).toBe('Test Admin')
    expect(result.current.isAuthenticated).toBe(true)
  })
  it('starts signed out and signs in with a valid admin account', async () => {
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    expect(result.current.isAuthenticated).toBe(false)
    await act(async () => { await result.current.login(input) })
    expect(result.current.user?.username).toBe('root')
    expect(result.current.isAuthenticated).toBe(true)
    expect(readStoredToken()).toBe('test-token')
    expect(document.cookie).toBe('')
    expect(localStorage.length).toBe(0)
  })

  it.each(['application', 'disabled'] as const)('rejects an unsuitable account: %s', async (kind) => {
    vi.mocked(authApi.login).mockResolvedValue({ accessToken: 'rejected', tokenType: 'bearer', expiresIn: 60,
      user: { ...admin, ...(kind === 'application' ? { role: 'application' as const } : { status: 'disabled' as const }) } })
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    await act(async () => { await expect(result.current.login(input)).rejects.toThrow('active administrator') })
    expect(result.current.isAuthenticated).toBe(false)
    expect(readStoredToken()).toBeNull()
  })

  it('does not authenticate when login fails', async () => {
    vi.mocked(authApi.login).mockRejectedValue(new Error('wrong password'))
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    await act(async () => { await expect(result.current.login(input)).rejects.toThrow('wrong password') })
    expect(result.current.user).toBeNull()
    expect(readStoredToken()).toBeNull()
  })

  it('cannot restore a logged-out user from a delayed /me response', async () => {
    const delayed = deferred<typeof admin>()
    vi.mocked(authApi.me).mockReturnValue(delayed.promise)
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    await act(async () => { await result.current.login(input) })
    act(() => result.current.logout())
    await act(async () => { delayed.resolve(admin) })
    expect(result.current).toMatchObject({ isAuthenticated: false, accessToken: null, user: null })
  })

  it('removes private user data when the token is invalidated outside the provider', async () => {
    const { result } = renderHook(useAuth, { wrapper: Wrapper, reactStrictMode: true })
    await act(async () => { await result.current.login(input) })
    act(() => clearStoredToken())
    expect(result.current).toMatchObject({ user: null, isAuthenticated: false })
  })
})
