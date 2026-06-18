import type { PropsWithChildren } from 'react'

interface SectionProps extends PropsWithChildren {
  title: string
  description?: string
}

export function Section({ children, description, title }: SectionProps) {
  return (
    <section className="space-y-4">
      <div>
        <h2 className="text-lg font-semibold text-slate-950">{title}</h2>
        {description ? <p className="mt-1 text-sm text-slate-500">{description}</p> : null}
      </div>
      {children}
    </section>
  )
}
