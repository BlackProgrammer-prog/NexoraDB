import { useMemo, useState } from 'react'
import { queryApi } from '../../query/services/queryApi'
import { QueryResultPanel } from '../../query/components/QueryResultPanel'
import type { QueryResult } from '../../query/types/query.types'
import { Badge } from '../../../shared/components/ui/Badge'
import { Card } from '../../../shared/components/ui/Card'
import { EmptyState } from '../../../shared/components/ui/EmptyState'
import type { Graph } from '../types/graph.types'

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

const ALGORITHMS: AlgorithmAction[] = [
  {
    name: 'AllDistances',
    kind: 'JOB',
    description: 'BFS distances from the first node.',
    requirement: 'oneNode',
    buildQuery: ({ graphId, sourceNodeId }) =>
      `RUN JOB AllDistances ON ${graphId} WITH source=${quoteValue(sourceNodeId)}, all=true, max_hops=4;`,
  },
  {
    name: 'AreConnected',
    kind: 'LOCK',
    description: 'Check whether the first two nodes are connected.',
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
    description: 'Suggest friends for the first node.',
    requirement: 'oneNode',
    buildQuery: ({ graphId, sourceNodeId }) =>
      `RUN LOCK FriendSuggestion ON ${graphId} WITH user=${quoteValue(
        sourceNodeId,
      )}, depth=2 LIMIT 20;`,
  },
  {
    name: 'GetFriends',
    kind: 'LOCK',
    description: 'Return direct friends of the first node.',
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
    description: 'Find mutual friends for the first two nodes.',
    requirement: 'twoNodes',
    buildQuery: ({ edgeType, graphId, sourceNodeId, targetNodeId }) =>
      `RUN LOCK MutualFriends ON ${graphId} WITH user1=${quoteValue(
        sourceNodeId,
      )}, user2=${quoteValue(targetNodeId)}${edgeTypeParam(edgeType)};`,
  },
  {
    name: 'Neighborhood',
    kind: 'LOCK',
    description: 'Explore the neighborhood around the first node.',
    requirement: 'oneNode',
    buildQuery: ({ graphId, sourceNodeId }) =>
      `RUN LOCK Neighborhood ON ${graphId} WITH node=${quoteValue(sourceNodeId)}, depth=2 LIMIT 50;`,
  },
  {
    name: 'NetworkStats',
    kind: 'LOCK',
    description: 'Read graph-level network statistics.',
    requirement: 'none',
    buildQuery: ({ graphId }) => `RUN LOCK NetworkStats ON ${graphId};`,
  },
  {
    name: 'PageRank',
    kind: 'JOB',
    description: 'Rank important nodes.',
    requirement: 'none',
    buildQuery: ({ graphId }) =>
      `RUN JOB PageRank ON ${graphId} WITH iterations=20, damping=0.85 RETURNS TOP 20;`,
  },
  {
    name: 'ShortestPath',
    kind: 'LOCK',
    description: 'Find a shortest path between the first two nodes.',
    requirement: 'twoNodes',
    buildQuery: ({ edgeType, graphId, sourceNodeId, targetNodeId }) =>
      `RUN LOCK ShortestPath ON ${graphId} WITH from=${quoteValue(
        sourceNodeId,
      )}, to=${quoteValue(targetNodeId)}${edgeTypeParam(edgeType)}, max_depth=6;`,
  },
]

export function GraphAlgorithmPanel({ graph }: GraphAlgorithmPanelProps) {
  const [activeAlgorithm, setActiveAlgorithm] = useState<string | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const [isLoading, setIsLoading] = useState(false)
  const [lastQuery, setLastQuery] = useState<string | null>(null)
  const [result, setResult] = useState<QueryResult | null>(null)

  const context = useMemo<AlgorithmContext | null>(() => {
    if (!graph) {
      return null
    }

    return {
      edgeType: graph.edges[0]?.label ?? null,
      graphId: graph.id,
      sourceNodeId: graph.nodes[0]?.id ?? null,
      targetNodeId: graph.nodes[1]?.id ?? null,
    }
  }, [graph])

  async function runAlgorithm(algorithm: AlgorithmAction) {
    if (!context) {
      return
    }

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

  return (
    <div className="space-y-4">
      <Card>
        <div className="space-y-4">
          <div className="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between">
            <div>
              <h3 className="text-base font-semibold text-slate-950">Algorithm runner</h3>
              <p className="mt-1 text-sm text-slate-500">
                Running against graph <span className="font-mono">{graph.id}</span>. Node parameters
                use the first nodes in this graph by default.
              </p>
            </div>
            <div className="flex flex-wrap gap-2">
              <Badge>{graph.nodes.length} nodes</Badge>
              <Badge>{graph.edges.length} edges</Badge>
              {context.edgeType ? <Badge>edge: {context.edgeType}</Badge> : null}
            </div>
          </div>

          <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
            {ALGORITHMS.map((algorithm) => {
              const disabledReason = getDisabledReason(algorithm, graph)
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

function getDisabledReason(algorithm: AlgorithmAction, graph: Graph) {
  if (algorithm.requirement === 'twoNodes' && graph.nodes.length < 2) {
    return 'Needs at least two nodes.'
  }

  if (algorithm.requirement === 'oneNode' && graph.nodes.length < 1) {
    return 'Needs at least one node.'
  }

  return null
}

function quoteValue(value: string | null) {
  return `'${(value ?? '').replace(/\\/g, '\\\\').replace(/'/g, "\\'")}'`
}

function edgeTypeParam(edgeType: string | null) {
  return edgeType ? `, edge_type=${quoteValue(edgeType)}` : ''
}
