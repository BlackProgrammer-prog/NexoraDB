export interface GraphNode {
  id: string
  label: string
  type?: string
  data: Record<string, unknown>
}

export interface GraphEdge {
  id: string
  source: string
  target: string
  label?: string
  data: Record<string, unknown>
}

export interface Graph {
  id: string
  name: string
  description?: string
  nodes: GraphNode[]
  edges: GraphEdge[]
  createdAt: string
  updatedAt: string
  stats?: {
    activeNodes: number
    activeEdges: number
    version: number
  }
}

export interface CreateGraphInput {
  name: string
  description?: string
}

export interface CreateGraphNodeInput {
  label: string
  type?: string
  data: Record<string, unknown>
}

export type UpdateGraphNodeInput = CreateGraphNodeInput

export interface CreateGraphEdgeInput {
  source: string
  target: string
  label?: string
  data: Record<string, unknown>
}

export type UpdateGraphEdgeInput = CreateGraphEdgeInput

export interface GraphVisualizationNode {
  id: string
  label: string
  type?: string | null
  collection?: string | null
}

export interface GraphVisualizationEdge {
  id: string
  source: string
  target: string
  label?: string | null
}

export interface GraphVisualizationData {
  graphId: string
  nodeCount: number
  edgeCount: number
  maxNodes: number
  nodes: GraphVisualizationNode[]
  edges: GraphVisualizationEdge[]
}

export interface GraphNodeDocument {
  nodeId: string
  nodeType?: string | null
  collection?: string | null
  document: Record<string, unknown>
}
