import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { formatDate } from '../../../shared/utils/formatDate'
import { stringifyJson } from '../../../shared/utils/json'
import type { DocumentRecord } from '../types/document.types'

interface DocumentCardProps {
  document: DocumentRecord
  onDelete?: (document: DocumentRecord) => void
  onEdit?: (document: DocumentRecord) => void
}

export function DocumentCard({ document, onDelete, onEdit }: DocumentCardProps) {
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
        {(onEdit || onDelete) ? (
          <div className="flex gap-2">
            {onEdit ? (
              <Button className="h-8 px-3" onClick={() => onEdit(document)} variant="secondary">
                Edit
              </Button>
            ) : null}
            {onDelete ? (
              <Button className="h-8 px-3" onClick={() => onDelete(document)} variant="ghost">
                Delete
              </Button>
            ) : null}
          </div>
        ) : null}
      </div>
    </Card>
  )
}
