import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { stringifyJson } from '../../../shared/utils/json'
import type { QueryResult } from '../types/query.types'
import { QueryExecutionStats } from './QueryExecutionStats'

const GraphVisualization = lazy(() =>
  import('../../graphs/components/GraphVisualization').then((module) => ({
    default: module.GraphVisualization,
  })),
)

interface QueryResultPanelProps {
  error: Error | null
  isIdle: boolean
  isLoading: boolean
  result: QueryResult | null
}

export function QueryResultPanel({
  error,
  isIdle,
  isLoading,
  result,
}: QueryResultPanelProps) {
  if (isLoading) {
    return <LoadingState label="Running query" />
  }

  if (error) {
    return <ErrorState message={error.message} />
  }

  if (isIdle || !result) {
    return (
      <EmptyState
        description="Run a NexoraQL statement to preview tabular rows and raw JSON output."
        title="No query has been executed"
      />
    )
  }

  const graphId = graphIdFromQueryResult(result.raw)

  return (
    <div className="space-y-4">
      <QueryExecutionStats executionTimeMs={result.executionTimeMs} />
      {result.rows.length ? (
        <div className="overflow-auto rounded-lg border border-slate-200">
          <table className="min-w-full divide-y divide-slate-200 text-left text-sm">
            <thead className="bg-slate-50 text-xs uppercase text-slate-500">
              <tr>
                {result.columns.map((column) => (
                  <th className="px-4 py-3 font-semibold" key={column}>
                    {column}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody className="divide-y divide-slate-100 bg-white text-slate-700">
              {result.rows.map((row, rowIndex) => (
                <tr key={rowIndex}>
                  {result.columns.map((column) => (
                    <td className="max-w-64 px-4 py-3 align-top" key={column}>
                      <span className="line-clamp-3 font-mono text-xs">
                        {String(row[column] ?? '')}
                      </span>
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <EmptyState description="The query completed but returned no rows." title="No rows" />
      )}
      <div>
        <h3 className="mb-2 text-sm font-semibold text-slate-950">Raw JSON</h3>
        <pre className="max-h-80 overflow-auto rounded-md bg-slate-950 p-4 text-xs text-green-100">
          {stringifyJson(result.raw)}
        </pre>
      </div>
      {graphId ? (
        <Suspense fallback={<div className="text-sm text-slate-500">Loading graph viewer…</div>}>
          <GraphVisualization graphId={graphId} refreshKey={result.executionTimeMs} />
        </Suspense>
      ) : null}
    </div>
  )
}

function graphIdFromQueryResult(raw: unknown) {
  if (!raw || typeof raw !== 'object' || !("statements" in raw)) return null
  const statements = (raw as { statements?: unknown }).statements
  if (!Array.isArray(statements)) return null
  const graphStatement = statements.find(
    (statement): statement is { algo: string; graph: string } =>
      Boolean(
        statement &&
          typeof statement === 'object' &&
          typeof (statement as { algo?: unknown }).algo === 'string' &&
          typeof (statement as { graph?: unknown }).graph === 'string',
      ),
  )
  return graphStatement?.graph ?? null
}
import { lazy, Suspense } from 'react'
