import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react'
import { authApi } from '../services/authApi'
import type { AdminUser, LoginInput, RegisterInput } from '../types/auth.types'

const TOKEN_STORAGE_KEY = 'nexoradb_admin_access_token'

interface AuthContextValue {
  user: AdminUser | null
  accessToken: string | null
  isAuthenticated: boolean
  isInitializing: boolean
  login: (input: LoginInput) => Promise<void>
  registerRootAdmin: (input: RegisterInput) => Promise<void>
  logout: () => void
}

const AuthContext = createContext<AuthContextValue | null>(null)

function readStoredToken() {
  return window.localStorage.getItem(TOKEN_STORAGE_KEY)
}

function persistToken(accessToken: string) {
  window.localStorage.setItem(TOKEN_STORAGE_KEY, accessToken)
}

function clearStoredToken() {
  window.localStorage.removeItem(TOKEN_STORAGE_KEY)
}

interface AuthProviderProps {
  children: ReactNode
}

export function AuthProvider({ children }: AuthProviderProps) {
  const [accessToken, setAccessToken] = useState<string | null>(() => readStoredToken())
  const [user, setUser] = useState<AdminUser | null>(null)
  const [isInitializing, setIsInitializing] = useState(Boolean(accessToken))

  const logout = useCallback(() => {
    clearStoredToken()
    setAccessToken(null)
    setUser(null)
    setIsInitializing(false)
  }, [])

  useEffect(() => {
    let isActive = true

    async function loadCurrentUser(token: string) {
      setIsInitializing(true)

      try {
        const currentUser = await authApi.me(token)

        if (isActive) {
          setUser(currentUser)
        }
      } catch {
        if (isActive) {
          clearStoredToken()
          setAccessToken(null)
          setUser(null)
        }
      } finally {
        if (isActive) {
          setIsInitializing(false)
        }
      }
    }

    if (!accessToken) {
      setIsInitializing(false)
      return undefined
    }

    void loadCurrentUser(accessToken)

    return () => {
      isActive = false
    }
  }, [accessToken])

  const login = useCallback(async (input: LoginInput) => {
    const response = await authApi.login(input)
    persistToken(response.accessToken)
    setAccessToken(response.accessToken)
    setUser(response.user)
  }, [])

  const registerRootAdmin = useCallback(
    async (input: RegisterInput) => {
      await authApi.register(input)
      await login({ username: 'root', password: input.password })
    },
    [login],
  )

  const value = useMemo<AuthContextValue>(
    () => ({
      accessToken,
      isAuthenticated: Boolean(accessToken && user),
      isInitializing,
      login,
      logout,
      registerRootAdmin,
      user,
    }),
    [accessToken, isInitializing, login, logout, registerRootAdmin, user],
  )

  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>
}

export function useAuth() {
  const context = useContext(AuthContext)

  if (context === null) {
    throw new Error('useAuth must be used inside AuthProvider')
  }

  return context
}
