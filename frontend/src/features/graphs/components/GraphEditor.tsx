import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import type { Graph, GraphEdge, GraphNode } from '../types/graph.types'
import { EdgeList } from './EdgeList'
import { NodeList } from './NodeList'

interface GraphEditorProps {
  graph: Graph | null
  onAddEdge: () => void
  onAddNode: () => void
  onDeleteEdge: (edge: GraphEdge) => void
  onDeleteNode: (node: GraphNode) => void
  onEditEdge: (edge: GraphEdge) => void
  onEditNode: (node: GraphNode) => void
}

export function GraphEditor({
  graph,
  onAddEdge,
  onAddNode,
  onDeleteEdge,
  onDeleteNode,
  onEditEdge,
  onEditNode,
}: GraphEditorProps) {
  if (!graph) {
    return (
      <Card>
        <div className="py-8 text-center">
          <h3 className="text-sm font-semibold text-slate-950">Select a graph</h3>
          <p className="mt-2 text-sm text-slate-500">
            Pick a graph to manage nodes and edges.
          </p>
        </div>
      </Card>
    )
  }

  return (
    <div className="space-y-6">
      <Card>
        <div className="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div>
            <h2 className="text-lg font-semibold text-slate-950">{graph.name}</h2>
            <p className="mt-1 text-sm text-slate-500">
              {graph.description || 'No description'}
            </p>
          </div>
          <div className="flex gap-2">
            <Button onClick={onAddNode} variant="secondary">
              Add node
            </Button>
            <Button disabled={graph.nodes.length < 2} onClick={onAddEdge} variant="secondary">
              Add edge
            </Button>
          </div>
        </div>
      </Card>
      <section className="space-y-3">
        <h3 className="text-base font-semibold text-slate-950">Nodes</h3>
        <NodeList nodes={graph.nodes} onDelete={onDeleteNode} onEdit={onEditNode} />
      </section>
      <section className="space-y-3">
        <h3 className="text-base font-semibold text-slate-950">Edges</h3>
        <EdgeList
          edges={graph.edges}
          nodes={graph.nodes}
          onDelete={onDeleteEdge}
          onEdit={onEditEdge}
        />
      </section>
    </div>
  )
}
