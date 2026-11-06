import {
  useCallback,
  useEffect,
  useMemo,
  useState,
  useSyncExternalStore,
  type ReactNode,
} from 'react'
import { authApi } from '../services/authApi'
import { clearStoredToken, persistToken, readStoredToken, subscribeToken } from '../services/authTokenStorage'
import type { AdminUser, LoginInput, RegisterInput } from '../types/auth.types'
import { AuthContext, type AuthContextValue } from './useAuth'

interface AuthProviderProps {
  children: ReactNode
}

export function AuthProvider({ children }: AuthProviderProps) {
  const accessToken = useSyncExternalStore(subscribeToken, readStoredToken, () => null)
  const [user, setUser] = useState<AdminUser | null>(null)
  const [isInitializing, setIsInitializing] = useState(Boolean(accessToken))

  const logout = useCallback(() => {
    const token = readStoredToken()
    if (token) void authApi.logout(token).catch(() => undefined)
    clearStoredToken()
    setUser(null)
    setIsInitializing(false)
  }, [])

  useEffect(() => {
    let isActive = true
    const controller = new AbortController()

    async function loadCurrentUser(token: string) {
      try {
        const currentUser = await authApi.me(token, controller.signal)

        if (isActive) {
          setUser(currentUser)
        }
      } catch {
        if (isActive) {
          clearStoredToken()
          setUser(null)
        }
      } finally {
        if (isActive) {
          setIsInitializing(false)
        }
      }
    }

    if (!accessToken) {
      return undefined
    }

    void loadCurrentUser(accessToken)

    return () => {
      isActive = false
      controller.abort()
    }
  }, [accessToken])

  const login = useCallback(async (input: LoginInput) => {
    const response = await authApi.login(input)
    if (response.user.status !== 'active') {
      throw new Error('An active account is required')
    }
    persistToken(response.accessToken, response.expiresIn)
    setIsInitializing(false)
    setUser(response.user)
  }, [])

  const registerRootAdmin = useCallback(
    async (input: RegisterInput) => {
      void input
      throw new Error('Owner enrollment is local only. Ask the database operator to initialize IAM.')
    },
    [],
  )

  const value = useMemo<AuthContextValue>(
    () => ({
      accessToken,
      isAuthenticated: Boolean(accessToken && user),
      isInitializing,
      login,
      logout,
      registerRootAdmin,
      user: accessToken ? user : null,
    }),
    [accessToken, isInitializing, login, logout, registerRootAdmin, user],
  )

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>
}
