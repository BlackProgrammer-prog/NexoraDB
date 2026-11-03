import { useMemo, useState } from 'react'
import { previewJson } from '../../utils/previewJson'

function Preview({ value }: { value: unknown }) {
  const text = useMemo(() => previewJson(value), [value])
  return <pre className="max-h-80 overflow-auto rounded-md bg-slate-950 p-4 text-xs text-green-100">{text}</pre>
}
export function JsonPreview({ value }: { value: unknown }) {
  const [open, setOpen] = useState(false)
  return <div>
    <button type="button" aria-expanded={open} className="mb-2 text-sm font-semibold text-slate-700" onClick={() => setOpen(!open)}>
      {open ? 'Hide JSON preview' : 'Show JSON preview'}
    </button>
    {open && <><p className="mb-2 text-xs text-slate-500">Bounded preview; large or deeply nested values may be truncated.</p><Preview value={value} /></>}
  </div>
}
