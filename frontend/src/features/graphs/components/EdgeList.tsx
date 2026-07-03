import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { stringifyJson } from '../../../shared/utils/json'
import type { GraphEdge, GraphNode } from '../types/graph.types'

interface EdgeListProps {
  edges: GraphEdge[]
  nodes: GraphNode[]
  onDelete: (edge: GraphEdge) => void
  onEdit: (edge: GraphEdge) => void
}

function getNodeLabel(nodes: GraphNode[], nodeId: string) {
  return nodes.find((node) => node.id === nodeId)?.label ?? nodeId
}

export function EdgeList({ edges, nodes, onDelete, onEdit }: EdgeListProps) {
  if (!edges.length) {
    return <p className="text-sm text-slate-500">No edges in this graph.</p>
  }

  return (
    <div className="grid gap-3 lg:grid-cols-2">
      {edges.map((edge) => (
        <Card className="p-4" key={edge.id}>
          <div className="space-y-3">
            <div>
              <h4 className="font-semibold text-slate-950">{edge.label || 'edge'}</h4>
              <p className="text-sm text-slate-500">
                {getNodeLabel(nodes, edge.source)} to {getNodeLabel(nodes, edge.target)}
              </p>
            </div>
            <pre className="max-h-24 overflow-auto rounded-md bg-slate-50 p-3 text-xs text-slate-700">
              {stringifyJson(edge.data)}
            </pre>
            <div className="flex gap-2">
              <Button className="h-8 px-3" onClick={() => onEdit(edge)} variant="secondary">
                Edit
              </Button>
              <Button className="h-8 px-3" onClick={() => onDelete(edge)} variant="ghost">
                Delete
              </Button>
            </div>
          </div>
        </Card>
      ))}
    </div>
  )
}
