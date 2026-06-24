import type { ReactNode } from 'react'

interface AuthPanelProps {
  children: ReactNode
}

export function AuthPanel({ children }: AuthPanelProps) {
  return (
    <div className="rounded-lg border border-slate-200 bg-white p-5 shadow-sm sm:p-6">
      {children}
    </div>
  )
}
