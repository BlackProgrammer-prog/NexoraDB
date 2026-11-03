import { createContext, useContext } from 'react'
import type { AdminUser, LoginInput, RegisterInput } from '../types/auth.types'

export interface AuthContextValue {
  user: AdminUser | null
  accessToken: string | null
  isAuthenticated: boolean
  isInitializing: boolean
  login: (input: LoginInput) => Promise<void>
  registerRootAdmin: (input: RegisterInput) => Promise<void>
  logout: () => void
}

export const AuthContext = createContext<AuthContextValue | null>(null)

export function useAuth() {
  const context = useContext(AuthContext)

  if (context === null) {
    throw new Error('useAuth must be used inside AuthProvider')
  }

  return context
}
