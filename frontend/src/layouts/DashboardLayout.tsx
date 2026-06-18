import type { ReactNode } from 'react'
import { cn } from '../shared/utils/cn'

export type AppPage = 'dashboard' | 'collections' | 'documents'

interface DashboardLayoutProps {
  activePage: AppPage
  children: ReactNode
  onNavigate: (page: AppPage) => void
}

const navItems: Array<{ label: string; page: AppPage }> = [
  { label: 'Dashboard', page: 'dashboard' },
  { label: 'Collections', page: 'collections' },
  { label: 'Documents', page: 'documents' },
]

export function DashboardLayout({
  activePage,
  children,
  onNavigate,
}: DashboardLayoutProps) {
  return (
    <div className="min-h-screen bg-slate-50 text-slate-950">
      <div className="flex min-h-screen flex-col lg:flex-row">
        <aside className="border-b border-slate-200 bg-white lg:w-64 lg:border-b-0 lg:border-r">
          <div className="flex h-16 items-center border-b border-slate-200 px-6">
            <div>
              <p className="text-base font-bold text-green-700">NexoraDB</p>
              <p className="text-xs text-slate-500">Admin panel</p>
            </div>
          </div>
          <nav className="flex gap-2 overflow-x-auto p-4 lg:flex-col">
            {navItems.map((item) => (
              <button
                className={cn(
                  'rounded-md px-3 py-2 text-left text-sm font-medium transition',
                  activePage === item.page
                    ? 'bg-green-50 text-green-700'
                    : 'text-slate-600 hover:bg-slate-100 hover:text-slate-950',
                )}
                key={item.page}
                onClick={() => onNavigate(item.page)}
                type="button"
              >
                {item.label}
              </button>
            ))}
          </nav>
        </aside>
        <div className="flex min-w-0 flex-1 flex-col">
          <header className="flex h-16 items-center justify-between border-b border-slate-200 bg-white px-6">
            {/* <p className="text-sm font-medium text-slate-600">FastAPI-ready frontend</p> */}
            {/* <span className="rounded-full bg-green-50 px-3 py-1 text-xs font-medium text-green-700 ring-1 ring-green-100">
              Mock API
            </span> */}
          </header>
          <main className="flex-1 p-4 sm:p-6 lg:p-8">{children}</main>
        </div>
      </div>
    </div>
  )
}
