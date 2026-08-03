import type { PropsWithChildren } from 'react'
import { HashRouter } from 'react-router-dom'
import { AuthProvider } from '../features/auth/context/AuthContext'

export function AppProviders({ children }: PropsWithChildren) {
  return (
    <HashRouter>
      <AuthProvider>{children}</AuthProvider>
    </HashRouter>
  )
}
