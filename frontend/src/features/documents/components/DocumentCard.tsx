import { Card } from '../../../shared/components/ui/Card'
import { formatDate } from '../../../shared/utils/formatDate'
import { stringifyJson } from '../../../shared/utils/json'
import type { DocumentRecord } from '../types/document.types'

interface DocumentCardProps {
  document: DocumentRecord
}

export function DocumentCard({ document }: DocumentCardProps) {
  return (
    <Card>
      <div className="flex flex-col gap-3">
        <div className="flex items-center justify-between gap-4">
          <h3 className="font-mono text-sm font-semibold text-slate-950">{document.id}</h3>
          <span className="text-xs text-slate-500">{formatDate(document.updatedAt)}</span>
        </div>
        <pre className="max-h-32 overflow-auto rounded-md bg-slate-50 p-3 text-xs text-slate-700">
          {stringifyJson(document.data)}
        </pre>
      </div>
    </Card>
  )
}
