import { USE_MOCK_API } from '../../../api/client/apiConfig'
import { httpClient } from '../../../api/client/httpClient'
import { graphEndpoints } from '../../../api/endpoints/graphEndpoints'
import { mockApiAdapter } from '../../../mocks/mockApiAdapter'
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
    if (USE_MOCK_API) {
      return mockApiAdapter.listGraphs()
    }

    return httpClient.get<Graph[]>(graphEndpoints.list)
  },

  createGraph(input: CreateGraphInput): Promise<Graph> {
    if (USE_MOCK_API) {
      return mockApiAdapter.createGraph(input)
    }

    return httpClient.post<Graph, CreateGraphInput>(graphEndpoints.create, input)
  },

  deleteGraph(graphId: string): Promise<void> {
    if (USE_MOCK_API) {
      return mockApiAdapter.deleteGraph(graphId)
    }

    return httpClient.delete<void>(graphEndpoints.delete(graphId))
  },

  createNode(graphId: string, input: CreateGraphNodeInput): Promise<Graph> {
    if (USE_MOCK_API) {
      return mockApiAdapter.createGraphNode(graphId, input)
    }

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
    if (USE_MOCK_API) {
      return mockApiAdapter.updateGraphNode(graphId, nodeId, input)
    }

    return httpClient.put<Graph, UpdateGraphNodeInput>(
      graphEndpoints.updateNode(graphId, nodeId),
      input,
    )
  },

  deleteNode(graphId: string, nodeId: string): Promise<Graph> {
    if (USE_MOCK_API) {
      return mockApiAdapter.deleteGraphNode(graphId, nodeId)
    }

    return httpClient.delete<Graph>(graphEndpoints.deleteNode(graphId, nodeId))
  },

  createEdge(graphId: string, input: CreateGraphEdgeInput): Promise<Graph> {
    if (USE_MOCK_API) {
      return mockApiAdapter.createGraphEdge(graphId, input)
    }

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
    if (USE_MOCK_API) {
      return mockApiAdapter.updateGraphEdge(graphId, edgeId, input)
    }

    return httpClient.put<Graph, UpdateGraphEdgeInput>(
      graphEndpoints.updateEdge(graphId, edgeId),
      input,
    )
  },

  deleteEdge(graphId: string, edgeId: string): Promise<Graph> {
    if (USE_MOCK_API) {
      return mockApiAdapter.deleteGraphEdge(graphId, edgeId)
    }

    return httpClient.delete<Graph>(graphEndpoints.deleteEdge(graphId, edgeId))
  },
}
