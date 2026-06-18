import type { PropsWithChildren } from 'react'
import { cn } from '../../utils/cn'

interface CardProps extends PropsWithChildren {
  className?: string
}

export function Card({ children, className }: CardProps) {
  return (
    <div className={cn('rounded-lg border border-slate-200 bg-white p-5 shadow-sm', className)}>
      {children}
    </div>
  )
}
