import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { useCollections } from '../hooks/useCollections'
import { CollectionCard } from './CollectionCard'

export function CollectionList() {
  const { data: collections, error, isLoading } = useCollections()

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
        <CollectionCard collection={collection} key={collection.name} />
      ))}
    </div>
  )
}
