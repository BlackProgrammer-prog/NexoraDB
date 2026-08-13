import { Badge } from '../../../shared/components/ui/Badge'
import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { formatDate } from '../../../shared/utils/formatDate'
import type { Graph } from '../types/graph.types'

interface GraphCardProps {
  graph: Graph
  isSelected: boolean
  onDelete: (graph: Graph) => void
  onSelect: (graph: Graph) => void
}

export function GraphCard({ graph, isSelected, onDelete, onSelect }: GraphCardProps) {
  const nodeCount = graph.stats?.activeNodes ?? graph.nodes.length
  const edgeCount = graph.stats?.activeEdges ?? graph.edges.length

  return (
    <Card className={isSelected ? 'border-green-300 bg-green-50/40' : undefined}>
      <div className="space-y-4">
        <div className="flex items-start justify-between gap-4">
          <div>
            <h3 className="font-semibold text-slate-950">{graph.name}</h3>
            <p className="mt-1 line-clamp-2 text-sm text-slate-500">
              {graph.description || 'No description'}
            </p>
          </div>
          <Badge>{nodeCount} nodes · {edgeCount} edges</Badge>
        </div>
        <p className="text-xs text-slate-500">Updated {formatDate(graph.updatedAt)}</p>
        <div className="flex gap-2">
          <Button className="flex-1" onClick={() => onSelect(graph)} variant="secondary">
            View
          </Button>
          <Button onClick={() => onDelete(graph)} variant="ghost">
            Delete
          </Button>
        </div>
      </div>
    </Card>
  )
}
