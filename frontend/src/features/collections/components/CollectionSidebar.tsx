import type { Collection } from '../types/collection.types'
import { cn } from '../../../shared/utils/cn'

interface CollectionSidebarProps {
  collections: Collection[] | null
  onSelect?: (collectionName: string) => void
  selectedCollectionName?: string
}

export function CollectionSidebar({
  collections,
  onSelect,
  selectedCollectionName,
}: CollectionSidebarProps) {

  return (
    <aside className="rounded-lg border border-slate-200 bg-white p-4">
      <h2 className="text-sm font-semibold text-slate-950">Collections</h2>
      <div className="mt-4 space-y-2">
        {(collections ?? []).map((collection) => (
          <button
            className={cn(
              'w-full rounded-md px-3 py-2 text-left text-sm text-slate-600 hover:bg-green-50 hover:text-green-700',
              selectedCollectionName === collection.name && 'bg-green-50 text-green-700',
            )}
            key={collection.name}
            onClick={() => onSelect?.(collection.name)}
            type="button"
          >
            {collection.name}
          </button>
        ))}
      </div>
    </aside>
  )
}
