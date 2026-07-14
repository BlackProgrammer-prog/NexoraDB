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

export interface UpdateGraphNodeInput extends CreateGraphNodeInput {}

export interface CreateGraphEdgeInput {
  source: string
  target: string
  label?: string
  data: Record<string, unknown>
}

export interface UpdateGraphEdgeInput extends CreateGraphEdgeInput {}
