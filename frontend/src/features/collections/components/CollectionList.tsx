import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { useCollections } from '../hooks/useCollections'
import type { Collection } from '../types/collection.types'
import { CollectionCard } from './CollectionCard'

interface CollectionListProps {
  collections?: Collection[] | null
  error?: Error | null
  isLoading?: boolean
  onDelete?: (collection: Collection) => void
  onEdit?: (collection: Collection) => void
  onView?: (collection: Collection) => void
}

export function CollectionList(props: CollectionListProps) {
  const internalState = useCollections()
  const collections = props.collections ?? internalState.data
  const error = props.error ?? internalState.error
  const isLoading = props.isLoading ?? internalState.isLoading

  if (isLoading) {
    return <LoadingState label="Loading collections" />
  }

  if (error) {
    return <ErrorState message={error.message} />
  }

  if (!collections?.length) {
    return (
      <EmptyState
        description="Collections from FastAPI will appear here once connected."
        title="No collections yet"
      />
    )
  }

  return (
    <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">
      {collections.map((collection) => (
        <CollectionCard
          collection={collection}
          key={collection.name}
          onDelete={props.onDelete}
          onEdit={props.onEdit}
          onView={props.onView}
        />
      ))}
    </div>
  )
}
