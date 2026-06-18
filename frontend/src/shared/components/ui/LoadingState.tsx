interface LoadingStateProps {
  label?: string
}

export function LoadingState({ label = 'Loading' }: LoadingStateProps) {
  return (
    <div className="rounded-lg border border-slate-200 bg-white p-6 text-sm text-slate-500">
      {label}
    </div>
  )
}
