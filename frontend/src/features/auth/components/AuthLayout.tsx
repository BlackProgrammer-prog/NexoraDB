import type { ReactNode } from 'react'

interface AuthLayoutProps {
  children: ReactNode
  eyebrow: string
  subtitle: string
  title: string
}

export function AuthLayout({ children, eyebrow, subtitle, title }: AuthLayoutProps) {
  return (
    <main className="min-h-screen bg-[#f7fbf8] text-slate-950">
      <div className="grid min-h-screen lg:grid-cols-[1.05fr_0.95fr]">
        <section className="relative hidden overflow-hidden bg-slate-950 lg:block">
          <img
            alt="Modern database workspace"
            className="absolute inset-0 h-full w-full object-cover opacity-70"
            src="/src/assets/hero.png"
          />
          <div className="absolute inset-0 bg-[linear-gradient(120deg,rgba(2,6,23,0.92),rgba(15,23,42,0.5),rgba(22,101,52,0.52))]" />
          <div className="relative flex h-full flex-col justify-between p-12">
            <div>
              <p className="text-lg font-bold text-white">NexoraDB</p>
              <p className="mt-1 text-sm text-green-100">Database admin workspace</p>
            </div>
            <div className="max-w-xl">
              <p className="text-sm font-medium uppercase tracking-[0.18em] text-green-200">
                Secure access
              </p>
              <h1 className="mt-4 text-5xl font-semibold leading-tight text-white">
                Manage collections with a calmer command center.
              </h1>
              <p className="mt-5 max-w-lg text-base leading-7 text-slate-100">
                Sign in to inspect documents, organize collections, and keep your data workflow focused.
              </p>
            </div>
          </div>
        </section>
        <section className="flex min-h-screen items-center justify-center px-5 py-10 sm:px-8">
          <div className="w-full max-w-md">
            <div className="mb-9 lg:hidden">
              <p className="text-lg font-bold text-green-700">NexoraDB</p>
              <p className="mt-1 text-sm text-slate-500">Database admin workspace</p>
            </div>
            <p className="text-sm font-semibold uppercase tracking-[0.16em] text-green-700">
              {eyebrow}
            </p>
            <h2 className="mt-3 text-3xl font-semibold text-slate-950">{title}</h2>
            <p className="mt-3 text-sm leading-6 text-slate-500">{subtitle}</p>
            <div className="mt-8">{children}</div>
          </div>
        </section>
      </div>
    </main>
  )
}
