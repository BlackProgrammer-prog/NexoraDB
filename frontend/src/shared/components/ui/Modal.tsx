import type { PropsWithChildren } from 'react'
import { Button } from './Button'

interface ModalProps extends PropsWithChildren {
  isOpen: boolean
  title: string
  onClose: () => void
}

export function Modal({ children, isOpen, onClose, title }: ModalProps) {
  if (!isOpen) {
    return null
  }

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/40 p-4">
      <div className="w-full max-w-lg rounded-lg bg-white shadow-xl">
        <div className="flex items-center justify-between border-b border-slate-200 px-5 py-4">
          <h2 className="text-base font-semibold text-slate-950">{title}</h2>
          <Button aria-label="Close modal" onClick={onClose} variant="ghost">
            Close
          </Button>
        </div>
        <div className="p-5">{children}</div>
      </div>
    </div>
  )
}
