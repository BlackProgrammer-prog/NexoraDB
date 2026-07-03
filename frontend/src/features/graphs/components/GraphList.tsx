import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import type { Graph } from '../types/graph.types'
import { GraphCard } from './GraphCard'

interface GraphListProps {
  error: Error | null
  graphs: Graph[] | null
  isLoading: boolean
  onDelete: (graph: Graph) => void
  onSelect: (graph: Graph) => void
  selectedGraphId?: string
}

export function GraphList({
  error,
  graphs,
  isLoading,
  onDelete,
  onSelect,
  selectedGraphId,
}: GraphListProps) {
  if (isLoading) {
    return <LoadingState label="Loading graphs" />
  }

  if (error) {
    return <ErrorState message={error.message} />
  }

  if (!graphs?.length) {
    return (
      <EmptyState
        description="Create a graph to start managing nodes and edges visually."
        title="No graphs yet"
      />
    )
  }

  return (
    <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3">
      {graphs.map((graph) => (
        <GraphCard
          graph={graph}
          isSelected={graph.id === selectedGraphId}
          key={graph.id}
          onDelete={onDelete}
          onSelect={onSelect}
        />
      ))}
    </div>
  )
}
