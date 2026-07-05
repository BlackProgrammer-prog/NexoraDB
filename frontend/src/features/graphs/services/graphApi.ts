import { httpClient } from '../../../api/client/httpClient'
import { graphEndpoints } from '../../../api/endpoints/graphEndpoints'
import type {
  CreateGraphEdgeInput,
  CreateGraphInput,
  CreateGraphNodeInput,
  Graph,
  UpdateGraphEdgeInput,
  UpdateGraphNodeInput,
} from '../types/graph.types'

export const graphApi = {
  listGraphs(): Promise<Graph[]> {
    return httpClient.get<Graph[]>(graphEndpoints.list)
  },

  createGraph(input: CreateGraphInput): Promise<Graph> {
    return httpClient.post<Graph, CreateGraphInput>(graphEndpoints.create, input)
  },

  deleteGraph(graphId: string): Promise<void> {
    return httpClient.delete<void>(graphEndpoints.delete(graphId))
  },

  createNode(graphId: string, input: CreateGraphNodeInput): Promise<Graph> {
    return httpClient.post<Graph, CreateGraphNodeInput>(
      graphEndpoints.createNode(graphId),
      input,
    )
  },

  updateNode(
    graphId: string,
    nodeId: string,
    input: UpdateGraphNodeInput,
  ): Promise<Graph> {
    return httpClient.put<Graph, UpdateGraphNodeInput>(
      graphEndpoints.updateNode(graphId, nodeId),
      input,
    )
  },

  deleteNode(graphId: string, nodeId: string): Promise<Graph> {
    return httpClient.delete<Graph>(graphEndpoints.deleteNode(graphId, nodeId))
  },

  createEdge(graphId: string, input: CreateGraphEdgeInput): Promise<Graph> {
    return httpClient.post<Graph, CreateGraphEdgeInput>(
      graphEndpoints.createEdge(graphId),
      input,
    )
  },

  updateEdge(
    graphId: string,
    edgeId: string,
    input: UpdateGraphEdgeInput,
  ): Promise<Graph> {
    return httpClient.put<Graph, UpdateGraphEdgeInput>(
      graphEndpoints.updateEdge(graphId, edgeId),
      input,
    )
  },

  deleteEdge(graphId: string, edgeId: string): Promise<Graph> {
    return httpClient.delete<Graph>(graphEndpoints.deleteEdge(graphId, edgeId))
  },
}
