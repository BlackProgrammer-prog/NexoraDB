export const graphEndpoints = {
  list: '/graphs',
  create: '/graphs',
  detail: (graphId: string) => `/graphs/${encodeURIComponent(graphId)}`,
  delete: (graphId: string) => `/graphs/${encodeURIComponent(graphId)}`,
  visualization: (graphId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/visualization`,
  nodeDocument: (graphId: string, nodeId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/nodes/${encodeURIComponent(nodeId)}/document`,
  createNode: (graphId: string) => `/graphs/${encodeURIComponent(graphId)}/nodes`,
  updateNode: (graphId: string, nodeId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/nodes/${encodeURIComponent(nodeId)}`,
  deleteNode: (graphId: string, nodeId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/nodes/${encodeURIComponent(nodeId)}`,
  createEdge: (graphId: string) => `/graphs/${encodeURIComponent(graphId)}/edges`,
  updateEdge: (graphId: string, edgeId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/edges/${encodeURIComponent(edgeId)}`,
  deleteEdge: (graphId: string, edgeId: string) =>
    `/graphs/${encodeURIComponent(graphId)}/edges/${encodeURIComponent(edgeId)}`,
}
