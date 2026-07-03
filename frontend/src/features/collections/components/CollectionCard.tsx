import { Badge } from '../../../shared/components/ui/Badge'
import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { formatDate } from '../../../shared/utils/formatDate'
import type { Collection } from '../types/collection.types'

interface CollectionCardProps {
  collection: Collection
  onDelete?: (collection: Collection) => void
  onEdit?: (collection: Collection) => void
  onView?: (collection: Collection) => void
}

export function CollectionCard({ collection, onDelete, onEdit, onView }: CollectionCardProps) {
  return (
    <Card>
      <div className="space-y-4">
        <div className="flex items-start justify-between gap-4">
          <div>
            <h3 className="font-semibold text-slate-950">{collection.name}</h3>
            <p className="mt-1 text-sm text-slate-500">
              Updated {formatDate(collection.updatedAt)}
            </p>
          </div>
          <Badge>{collection.documentCount} docs</Badge>
        </div>
        <div className="flex flex-wrap gap-2">
          {onView ? (
            <Button className="h-8 px-3" onClick={() => onView(collection)} variant="secondary">
              View docs
            </Button>
          ) : null}
          {onEdit ? (
            <Button className="h-8 px-3" onClick={() => onEdit(collection)} variant="secondary">
              Rename
            </Button>
          ) : null}
          {onDelete ? (
            <Button className="h-8 px-3" onClick={() => onDelete(collection)} variant="ghost">
              Delete
            </Button>
          ) : null}
        </div>
      </div>
    </Card>
  )
}
