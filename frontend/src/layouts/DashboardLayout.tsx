import { useState } from 'react'
import { NavLink, Outlet, useNavigate } from 'react-router-dom'
import { useAuth } from '../features/auth/context/AuthContext'
import { cn } from '../shared/utils/cn'
import { formatDate } from '../shared/utils/formatDate'

export type AppPage = 'dashboard' | 'collections' | 'documents' | 'graphs' | 'query' | 'app-tokens'

const navItems: Array<{ label: string; path: string }> = [
  { label: 'Dashboard', path: '/dashboard' },
  { label: 'Collections', path: '/collections' },
  { label: 'Documents', path: '/documents' },
  { label: 'Graphs', path: '/graphs' },
  { label: 'Query editor', path: '/query' },
  { label: 'App tokens', path: '/app-tokens' },
]

export function DashboardLayout() {
  const navigate = useNavigate()
  const { logout, user } = useAuth()
  const [isProfileOpen, setIsProfileOpen] = useState(false)

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
            <div className="relative flex items-center gap-3">
              <button
                className="flex items-center gap-3 rounded-md px-2 py-1.5 text-left transition hover:bg-slate-100"
                onClick={() => setIsProfileOpen((value) => !value)}
                type="button"
              >
                <span className="flex h-9 w-9 items-center justify-center rounded-full bg-green-100 text-sm font-bold text-green-700">
                  {profileInitials(user?.displayName ?? user?.username ?? 'Admin')}
                </span>
                <span className="hidden sm:block">
                  <span className="block text-sm font-semibold text-slate-800">
                    {user?.displayName ?? user?.username ?? 'Admin'}
                  </span>
                  <span className="block text-xs text-slate-500">{user?.email ?? user?.role}</span>
                </span>
              </button>
              <button
                className="rounded-md border border-slate-200 px-3 py-1.5 text-sm font-medium text-slate-600 transition hover:bg-slate-100 hover:text-slate-950"
                onClick={handleLogout}
                type="button"
              >
                Sign out
              </button>
              {isProfileOpen && user ? (
                <div className="absolute right-0 top-12 z-50 w-80 rounded-lg border border-slate-200 bg-white p-4 shadow-xl">
                  <div className="border-b border-slate-100 pb-3">
                    <p className="font-semibold text-slate-950">{user.displayName}</p>
                    <p className="mt-1 text-sm text-slate-500">@{user.username}</p>
                  </div>
                  <dl className="mt-3 grid grid-cols-[7rem_1fr] gap-x-3 gap-y-2 text-sm">
                    <dt className="text-slate-500">Email</dt>
                    <dd className="break-all text-slate-800">{user.email ?? 'Not set'}</dd>
                    <dt className="text-slate-500">Role</dt>
                    <dd className="capitalize text-slate-800">{user.role}</dd>
                    <dt className="text-slate-500">Status</dt>
                    <dd className="capitalize text-green-700">{user.status}</dd>
                    <dt className="text-slate-500">Created</dt>
                    <dd className="text-slate-800">{formatDate(user.createdAt)}</dd>
                    <dt className="text-slate-500">Last login</dt>
                    <dd className="text-slate-800">
                      {user.lastLoginAt ? formatDate(user.lastLoginAt) : 'Not available'}
                    </dd>
                  </dl>
                </div>
              ) : null}
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

function profileInitials(name: string) {
  return name
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0]?.toUpperCase())
    .join('') || 'A'
}
