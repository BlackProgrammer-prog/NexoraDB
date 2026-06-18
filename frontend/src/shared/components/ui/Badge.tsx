import type { PropsWithChildren } from 'react'
import { cn } from '../../utils/cn'

interface BadgeProps extends PropsWithChildren {
  className?: string
}

export function Badge({ children, className }: BadgeProps) {
  return (
    <span
      className={cn(
        'inline-flex items-center rounded-full bg-green-50 px-2.5 py-1 text-xs font-medium text-green-700 ring-1 ring-green-100',
        className,
      )}
    >
      {children}
    </span>
  )
}
