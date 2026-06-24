import type { InputHTMLAttributes } from 'react'
import { Input } from '../../../shared/components/ui/Input'

interface AuthFieldProps extends InputHTMLAttributes<HTMLInputElement> {
  label: string
}

export function AuthField({ id, label, ...props }: AuthFieldProps) {
  return (
    <label className="block" htmlFor={id}>
      <span className="text-sm font-medium text-slate-700">{label}</span>
      <Input className="mt-2 h-11" id={id} {...props} />
    </label>
  )
}
