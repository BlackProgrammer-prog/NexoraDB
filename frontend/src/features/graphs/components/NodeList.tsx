import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { stringifyJson } from '../../../shared/utils/json'
import type { GraphNode } from '../types/graph.types'

interface NodeListProps {
  nodes: GraphNode[]
  onDelete: (node: GraphNode) => void
  onEdit: (node: GraphNode) => void
}

export function NodeList({ nodes, onDelete, onEdit }: NodeListProps) {
  if (!nodes.length) {
    return <p className="text-sm text-slate-500">No nodes in this graph.</p>
  }

  return (
    <div className="grid gap-3 lg:grid-cols-2">
      {nodes.map((node) => (
        <Card className="p-4" key={node.id}>
          <div className="space-y-3">
            <div>
              <h4 className="font-semibold text-slate-950">{node.label}</h4>
              <p className="font-mono text-xs text-slate-500">{node.id}</p>
              {node.type ? <p className="text-xs text-green-700">{node.type}</p> : null}
            </div>
            <pre className="max-h-24 overflow-auto rounded-md bg-slate-50 p-3 text-xs text-slate-700">
              {stringifyJson(node.data)}
            </pre>
            <div className="flex gap-2">
              <Button className="h-8 px-3" onClick={() => onEdit(node)} variant="secondary">
                Edit
              </Button>
              <Button className="h-8 px-3" onClick={() => onDelete(node)} variant="ghost">
                Delete
              </Button>
            </div>
          </div>
        </Card>
      ))}
    </div>
  )
}
