interface EmptyStateProps {
  title: string
  description?: string
}

export function EmptyState({ description, title }: EmptyStateProps) {
  return (
    <div className="rounded-lg border border-dashed border-slate-300 bg-white p-8 text-center">
      <h3 className="text-sm font-semibold text-slate-900">{title}</h3>
      {description ? <p className="mt-2 text-sm text-slate-500">{description}</p> : null}
    </div>
  )
}
