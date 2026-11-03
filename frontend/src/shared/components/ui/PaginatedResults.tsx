import { useState, type ReactNode } from 'react'
import { Button } from './Button'

interface Props<T> {
  items: T[]
  pageSize?: number
  children: (items: T[]) => ReactNode
}

// Bound DOM creation even when a legacy endpoint returns an entire result set.
// This is client pagination; the network response is capped separately.
export function PaginatedResults<T>({ items, pageSize = 50, children }: Props<T>) {
  const [requestedPage, setPage] = useState(0)
  const pages = Math.max(1, Math.ceil(items.length / pageSize))
  const page = Math.min(requestedPage, pages - 1)
  const start = page * pageSize
  return <div className="space-y-3">
    {children(items.slice(start, start + pageSize))}
    {items.length > pageSize && <nav aria-label="Results pagination" className="flex items-center gap-3 text-sm">
      <Button variant="secondary" disabled={page === 0} onClick={() => setPage(page - 1)}>Previous</Button>
      <span aria-live="polite">{start + 1}–{Math.min(start + pageSize, items.length)} of {items.length}</span>
      <Button variant="secondary" disabled={page + 1 >= pages} onClick={() => setPage(page + 1)}>Next</Button>
    </nav>}
  </div>
}
