import { Badge } from '../../../shared/components/ui/Badge'
import { Card } from '../../../shared/components/ui/Card'
import { formatDate } from '../../../shared/utils/formatDate'
import type { Collection } from '../types/collection.types'

interface CollectionCardProps {
  collection: Collection
}

export function CollectionCard({ collection }: CollectionCardProps) {
  return (
    <Card>
      <div className="flex items-start justify-between gap-4">
        <div>
          <h3 className="font-semibold text-slate-950">{collection.name}</h3>
          <p className="mt-1 text-sm text-slate-500">
            Updated {formatDate(collection.updatedAt)}
          </p>
        </div>
        <Badge>{collection.documentCount} docs</Badge>
      </div>
    </Card>
  )
}
