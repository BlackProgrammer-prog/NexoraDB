import { useEffect, useMemo, useState } from 'react'
import { QueryResultPanel } from '../../query/components/QueryResultPanel'
import { queryApi } from '../../query/services/queryApi'
import type { QueryResult } from '../../query/types/query.types'
import { Badge } from '../../../shared/components/ui/Badge'
import { Card } from '../../../shared/components/ui/Card'
import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { graphApi } from '../services/graphApi'
import type { Graph, GraphVisualizationData } from '../types/graph.types'

type AlgorithmKind = 'LOCK' | 'JOB'
type Requirement = 'none' | 'oneNode' | 'twoNodes'

interface AlgorithmContext {
  edgeType: string | null
  graphId: string
  sourceNodeId: string | null
  targetNodeId: string | null
}

interface AlgorithmAction {
  name: string
  kind: AlgorithmKind
  description: string
  requirement: Requirement
  buildQuery: (context: AlgorithmContext) => string
}

interface GraphAlgorithmPanelProps {
  graph: Graph | null
}

interface LoadedTopology {
  data: GraphVisualizationData
  graphId: string
}

const SELECT_CLASS_NAME =
  'h-10 w-full rounded-md border border-slate-200 bg-white px-3 text-sm text-slate-900 outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100 disabled:cursor-not-allowed disabled:bg-slate-100 disabled:text-slate-400'

const ALGORITHMS: AlgorithmAction[] = [
  {
    name: 'BetweennessCentrality',
    kind: 'JOB',
    description: 'Rank the bridge nodes that connect different parts of the graph.',
    requirement: 'none',
    buildQuery: ({ graphId }) =>
      `RUN JOB BetweennessCentrality ON ${graphId} RETURNS TOP 20;`,
  },
  {
    name: 'AllDistances',
    kind: 'JOB',
    description: 'BFS distances from the selected source node.',
    requirement: 'oneNode',
    buildQuery: ({ graphId, sourceNodeId }) =>
      `RUN JOB AllDistances ON ${graphId} WITH source=${quoteValue(
        sourceNodeId,
      )};`,
  },
  {
    name: 'AreConnected',
    kind: 'LOCK',
    description: 'Check whether the selected source and target are connected.',
    requirement: 'twoNodes',
    buildQuery: ({ edgeType, graphId, sourceNodeId, targetNodeId }) =>
      `RUN LOCK AreConnected ON ${graphId} WITH user1=${quoteValue(
        sourceNodeId,
      )}, user2=${quoteValue(targetNodeId)}${edgeTypeParam(edgeType)};`,
  },
  {
    name: 'CommunityDetection',
    kind: 'JOB',
    description: 'Find communities and return member samples.',
    requirement: 'none',
    buildQuery: ({ graphId }) =>
      `RUN JOB CommunityDetection ON ${graphId} WITH max_iterations=10, min_community_size=2, members=true;`,
  },
  {
    name: 'ConnectedComponents',
    kind: 'JOB',
    description: 'Compute connected components.',
    requirement: 'none',
    buildQuery: ({ graphId }) => `RUN JOB ConnectedComponents ON ${graphId};`,
  },
  {
    name: 'FriendSuggestion',
    kind: 'LOCK',
    description: 'Suggest friends for the selected source node.',
    requirement: 'oneNode',
    buildQuery: ({ edgeType, graphId, sourceNodeId }) =>
      `RUN LOCK FriendSuggestion ON ${graphId} WITH user=${quoteValue(
        sourceNodeId,
      )}${edgeTypeParam(edgeType)} LIMIT 20;`,
  },
  {
    name: 'InfluenceMaximization',
    kind: 'JOB',
    description: 'Choose seed nodes that maximize information spread.',
    requirement: 'none',
    buildQuery: ({ graphId }) =>
      `RUN JOB InfluenceMaximization ON ${graphId} WITH k=5, simulations=20, probability=0.1;`,
  },
  {
    name: 'GetFriends',
    kind: 'LOCK',
    description: 'Return direct friends of the selected source node.',
    requirement: 'oneNode',
    buildQuery: ({ edgeType, graphId, sourceNodeId }) =>
      `RUN LOCK GetFriends ON ${graphId} WITH user=${quoteValue(sourceNodeId)}${edgeTypeParam(
        edgeType,
      )} LIMIT 20;`,
  },
  {
    name: 'MostConnected',
    kind: 'LOCK',
    description: 'Return the most connected nodes.',
    requirement: 'none',
    buildQuery: ({ graphId }) => `RUN LOCK MostConnected ON ${graphId} LIMIT 20;`,
  },
  {
    name: 'MutualFriends',
    kind: 'LOCK',
    description: 'Find mutual friends for the selected source and target.',
    requirement: 'twoNodes',
    buildQuery: ({ edgeType, graphId, sourceNodeId, targetNodeId }) =>
      `RUN LOCK MutualFriends ON ${graphId} WITH user1=${quoteValue(
        sourceNodeId,
      )}, user2=${quoteValue(targetNodeId)}${edgeTypeParam(edgeType)};`,
  },
  {
    name: 'NetworkStats',
    kind: 'LOCK',
    description: 'Read graph-level network statistics.',
    requirement: 'none',
    buildQuery: ({ graphId }) => `RUN LOCK NetworkStats ON ${graphId};`,
  },
  {
    name: 'ShortestPath',
    kind: 'LOCK',
    description: 'Find a shortest path between the selected source and target.',
    requirement: 'twoNodes',
    buildQuery: ({ edgeType, graphId, sourceNodeId, targetNodeId }) =>
      `RUN LOCK ShortestPath ON ${graphId} WITH from=${quoteValue(
        sourceNodeId,
      )}, to=${quoteValue(targetNodeId)}${edgeTypeParam(edgeType)};`,
  },
]

export function GraphAlgorithmPanel({ graph }: GraphAlgorithmPanelProps) {
  const [activeAlgorithm, setActiveAlgorithm] = useState<string | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const [isLoading, setIsLoading] = useState(false)
  const [lastQuery, setLastQuery] = useState<string | null>(null)
  const [loadedTopology, setLoadedTopology] = useState<LoadedTopology | null>(null)
  const [loadingGraphId, setLoadingGraphId] = useState<string | null>(null)
  const [result, setResult] = useState<QueryResult | null>(null)
  const [selectedEdgeType, setSelectedEdgeType] = useState<string | null>(null)
  const [sourceNodeId, setSourceNodeId] = useState<string | null>(null)
  const [targetNodeId, setTargetNodeId] = useState<string | null>(null)
  const [topologyError, setTopologyError] = useState<Error | null>(null)

  const graphId = graph?.id ?? null
  const graphVersion = graph?.stats?.version ?? 0

  useEffect(() => {
    if (!graphId) return undefined

    let active = true
    queueMicrotask(() => {
      if (!active) return
      setLoadingGraphId(graphId)
      setTopologyError(null)
    })

    void graphApi
      .getVisualization(graphId)
      .then((data) => {
        if (!active) return
        setLoadedTopology({ data, graphId })
      })
      .catch((loadError: unknown) => {
        if (!active) return
        setTopologyError(
          loadError instanceof Error
            ? loadError
            : new Error('Could not load graph nodes for algorithms'),
        )
      })
      .finally(() => {
        if (active) setLoadingGraphId(null)
      })

    return () => {
      active = false
    }
  }, [graphId, graphVersion])

  const topology =
    graph && loadedTopology?.graphId === graph.id ? loadedTopology.data : null

  const availableNodes = useMemo(() => {
    if (topology) return topology.nodes
    return graph?.nodes ?? []
  }, [graph, topology])

  const edgeTypes = useMemo(() => {
    const labels = topology
      ? topology.edges.map((edge) => edge.label)
      : (graph?.edges.map((edge) => edge.label) ?? [])

    return [...new Set(labels.filter((label): label is string => Boolean(label)))].sort()
  }, [graph, topology])

  const activeSourceNodeId =
    sourceNodeId && availableNodes.some((node) => node.id === sourceNodeId)
      ? sourceNodeId
      : (availableNodes[0]?.id ?? null)
  const activeTargetNodeId =
    targetNodeId &&
    targetNodeId !== activeSourceNodeId &&
    availableNodes.some((node) => node.id === targetNodeId)
      ? targetNodeId
      : (availableNodes.find((node) => node.id !== activeSourceNodeId)?.id ?? null)
  const activeEdgeType =
    selectedEdgeType && edgeTypes.includes(selectedEdgeType)
      ? selectedEdgeType
      : (edgeTypes[0] ?? null)
  const isTopologyLoading = loadingGraphId === graphId

  const context = useMemo<AlgorithmContext | null>(() => {
    if (!graph) return null

    return {
      edgeType: activeEdgeType,
      graphId: graph.id,
      sourceNodeId: activeSourceNodeId,
      targetNodeId: activeTargetNodeId,
    }
  }, [activeEdgeType, activeSourceNodeId, activeTargetNodeId, graph])

  function changeSourceNode(nextSourceNodeId: string) {
    setSourceNodeId(nextSourceNodeId)
    if (nextSourceNodeId === activeTargetNodeId) {
      setTargetNodeId(
        availableNodes.find((node) => node.id !== nextSourceNodeId)?.id ?? null,
      )
    }
  }

  async function runAlgorithm(algorithm: AlgorithmAction) {
    if (!context) return

    const query = algorithm.buildQuery(context)
    setActiveAlgorithm(algorithm.name)
    setError(null)
    setIsLoading(true)
    setLastQuery(query)

    try {
      setResult(await queryApi.executeQuery({ query }))
    } catch (runError) {
      setResult(null)
      setError(runError instanceof Error ? runError : new Error('Unexpected algorithm error'))
    } finally {
      setIsLoading(false)
    }
  }

  if (!graph || !context) {
    return (
      <EmptyState
        description="Select a graph first, then run graph algorithms through NexoraQL."
        title="No graph selected"
      />
    )
  }

  const nodeCount = topology?.nodeCount ?? graph.stats?.activeNodes ?? availableNodes.length
  const edgeCount = topology?.edgeCount ?? graph.stats?.activeEdges ?? graph.edges.length

  return (
    <div className="space-y-4">
      <Card>
        <div className="space-y-4">
          <div className="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <h3 className="text-base font-semibold text-slate-950">Algorithm inputs</h3>
              <p className="mt-1 text-sm text-slate-500">
                Select the nodes used by node-based algorithms on graph{' '}
                <span className="font-mono">{graph.id}</span>.
              </p>
            </div>
            <div className="flex flex-wrap gap-2">
              <Badge>{nodeCount} nodes</Badge>
              <Badge>{edgeCount} edges</Badge>
              {activeEdgeType ? <Badge>edge: {activeEdgeType}</Badge> : null}
            </div>
          </div>

          {topologyError ? <ErrorState message={topologyError.message} /> : null}

          <div className="grid gap-4 md:grid-cols-3">
            <label className="space-y-2 text-sm font-medium text-slate-700">
              <span>Source node</span>
              <select
                className={SELECT_CLASS_NAME}
                disabled={isTopologyLoading || availableNodes.length === 0}
                onChange={(event) => changeSourceNode(event.target.value)}
                value={activeSourceNodeId ?? ''}
              >
                {availableNodes.length === 0 ? <option value="">No nodes available</option> : null}
                {availableNodes.map((node) => (
                  <option key={node.id} value={node.id}>
                    {nodeOptionLabel(node.label, node.id)}
                  </option>
                ))}
              </select>
            </label>

            <label className="space-y-2 text-sm font-medium text-slate-700">
              <span>Target node</span>
              <select
                className={SELECT_CLASS_NAME}
                disabled={isTopologyLoading || availableNodes.length < 2}
                onChange={(event) => setTargetNodeId(event.target.value)}
                value={activeTargetNodeId ?? ''}
              >
                {availableNodes.length < 2 ? <option value="">Needs two nodes</option> : null}
                {availableNodes
                  .filter((node) => node.id !== activeSourceNodeId)
                  .map((node) => (
                    <option key={node.id} value={node.id}>
                      {nodeOptionLabel(node.label, node.id)}
                    </option>
                  ))}
              </select>
            </label>

            <label className="space-y-2 text-sm font-medium text-slate-700">
              <span>Edge type</span>
              <select
                className={SELECT_CLASS_NAME}
                disabled={isTopologyLoading || edgeTypes.length === 0}
                onChange={(event) => setSelectedEdgeType(event.target.value || null)}
                value={activeEdgeType ?? ''}
              >
                {edgeTypes.length === 0 ? <option value="">All edge types</option> : null}
                {edgeTypes.map((edgeType) => (
                  <option key={edgeType} value={edgeType}>
                    {edgeType}
                  </option>
                ))}
              </select>
            </label>
          </div>

          {isTopologyLoading ? (
            <p className="text-xs text-slate-500">Loading graph nodes…</p>
          ) : null}
        </div>
      </Card>

      <Card>
        <div className="space-y-4">
          <div>
            <h3 className="text-base font-semibold text-slate-950">Algorithm runner</h3>
            <p className="mt-1 text-sm text-slate-500">
              Algorithms use the selected inputs above. Graph-level algorithms do not require a
              node selection.
            </p>
          </div>

          <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
            {ALGORITHMS.map((algorithm) => {
              const disabledReason = getDisabledReason(
                algorithm,
                context,
                availableNodes.length,
                isTopologyLoading,
              )
              const isRunning = isLoading && activeAlgorithm === algorithm.name

              return (
                <button
                  className="rounded-lg border border-slate-200 bg-white p-4 text-left transition hover:border-green-300 hover:bg-green-50 disabled:cursor-not-allowed disabled:opacity-55 disabled:hover:border-slate-200 disabled:hover:bg-white"
                  disabled={Boolean(disabledReason) || isLoading}
                  key={algorithm.name}
                  onClick={() => void runAlgorithm(algorithm)}
                  title={disabledReason ?? algorithm.description}
                  type="button"
                >
                  <div className="flex items-start justify-between gap-3">
                    <span className="font-mono text-sm font-semibold text-slate-950">
                      {algorithm.name}
                    </span>
                    <Badge>{algorithm.kind}</Badge>
                  </div>
                  <p className="mt-2 text-xs leading-5 text-slate-500">
                    {isRunning ? 'Running...' : disabledReason ?? algorithm.description}
                  </p>
                </button>
              )
            })}
          </div>
        </div>
      </Card>

      {lastQuery ? (
        <div>
          <h3 className="mb-2 text-sm font-semibold text-slate-950">Generated NexoraQL</h3>
          <pre className="overflow-auto rounded-md bg-slate-950 p-4 text-xs text-green-100">
            {lastQuery}
          </pre>
        </div>
      ) : null}

      <QueryResultPanel error={error} isIdle={!lastQuery} isLoading={isLoading} result={result} />
    </div>
  )
}

function getDisabledReason(
  algorithm: AlgorithmAction,
  context: AlgorithmContext,
  nodeCount: number,
  isTopologyLoading: boolean,
) {
  if (algorithm.requirement !== 'none' && isTopologyLoading) {
    return 'Loading graph nodes.'
  }

  if (algorithm.requirement === 'twoNodes' && nodeCount < 2) {
    return 'Needs at least two nodes.'
  }

  if (algorithm.requirement === 'twoNodes' && !context.targetNodeId) {
    return 'Select a target node.'
  }

  if (algorithm.requirement === 'oneNode' && !context.sourceNodeId) {
    return 'Select a source node.'
  }

  return null
}

function nodeOptionLabel(label: string, id: string) {
  return label && label !== id ? `${label} (${id})` : id
}

function quoteValue(value: string | null) {
  return `'${(value ?? '').replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`
}

function edgeTypeParam(edgeType: string | null) {
  return edgeType ? `, edge_type=${quoteValue(edgeType)}` : ''
}
