import { NavLink, Outlet, useNavigate } from 'react-router-dom'
import { useAuth } from '../features/auth/context/AuthContext'
import { cn } from '../shared/utils/cn'

export type AppPage = 'dashboard' | 'collections' | 'documents' | 'graphs' | 'query'

const navItems: Array<{ label: string; path: string }> = [
  { label: 'Dashboard', path: '/dashboard' },
  { label: 'Collections', path: '/collections' },
  { label: 'Documents', path: '/documents' },
  { label: 'Graphs', path: '/graphs' },
  { label: 'Query editor', path: '/query' },
]

export function DashboardLayout() {
  const navigate = useNavigate()
  const { logout, user } = useAuth()

  function handleLogout() {
    logout()
    navigate('/login', { replace: true })
  }

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
              <NavLink
                className={({ isActive }) =>
                  cn(
                    'rounded-md px-3 py-2 text-left text-sm font-medium transition',
                    isActive
                      ? 'bg-green-50 text-green-700'
                      : 'text-slate-600 hover:bg-slate-100 hover:text-slate-950',
                  )
                }
                key={item.path}
                to={item.path}
              >
                {item.label}
              </NavLink>
            ))}
          </nav>
        </aside>
        <div className="flex min-w-0 flex-1 flex-col">
          <header className="flex h-16 items-center justify-between border-b border-slate-200 bg-white px-6">
            <p className="text-sm font-medium text-slate-600">NexoraDB Console</p>
            <div className="flex items-center gap-3">
              <span className="hidden text-sm text-slate-500 sm:inline">
                {user?.username ?? 'Admin'}
              </span>
              <button
                className="rounded-md border border-slate-200 px-3 py-1.5 text-sm font-medium text-slate-600 transition hover:bg-slate-100 hover:text-slate-950"
                onClick={handleLogout}
                type="button"
              >
                Sign out
              </button>
            </div>
          </header>
          <main className="flex-1 p-4 sm:p-6 lg:p-8">
            <Outlet />
          </main>
        </div>
      </div>
    </div>
  )
}
