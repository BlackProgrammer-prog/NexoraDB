import { lazy, Suspense } from 'react'
import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { JsonPreview } from '../../../shared/components/ui/JsonPreview'
import { PaginatedResults } from '../../../shared/components/ui/PaginatedResults'
import type { QueryMode, QueryResult } from '../types/query.types'
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
  mode?: QueryMode
}

export function QueryResultPanel({
  error,
  isIdle,
  isLoading,
  result,
  mode = 'buffered',
}: QueryResultPanelProps) {
  if (isLoading && !result) {
    return <LoadingState label="Running query" />
  }

  if (error && (!result || (mode === 'buffered' && !result.statementErrors?.length))) {
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
      {error && <ErrorState message={`${error.message} Displayed rows are an incomplete preview.`} />}
      {isLoading && <p role="status" className="text-sm text-slate-600">Receiving results...</p>}
      {mode === 'buffered' ? <QueryExecutionStats executionTimeMs={result.executionTimeMs} />
        : <p aria-live="polite" className="text-sm text-slate-600">{result.totalRows ?? result.rows.length} rows received. Showing {result.rows.length} rows{mode === 'cursor' ? ' from the current server page' : ' in this preview'}.</p>}
      {result.previewTruncated && <p className="text-sm text-amber-700">Preview is limited to 1,000 rows and 8 MiB of encoded row data. Earlier rows were discarded.</p>}
      {result.rows.length ? (
        <PaginatedResults key={result.pageSequence ?? 0} items={result.rows}>{(page) => <div className="overflow-auto rounded-lg border border-slate-200">
          <table className="min-w-full divide-y divide-slate-200 text-left text-sm">
            <thead className="bg-slate-50 text-xs uppercase text-slate-500">
              <tr>
                {result.columns.slice(0, 50).map((column) => (
                  <th className="px-4 py-3 font-semibold" key={column}>
                    {column}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody className="divide-y divide-slate-100 bg-white text-slate-700">
              {page.map((row, rowIndex) => (
                <tr key={rowIndex}>
                  {result.columns.slice(0, 50).map((column) => (
                    <td className="max-w-64 px-4 py-3 align-top" key={column}>
                      <span className="line-clamp-3 font-mono text-xs">
                        {formatCell(row[column])}
                      </span>
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>}</PaginatedResults>
      ) : (
        <EmptyState description="The query completed but returned no rows." title="No rows" />
      )}
      {result.columns.length > 50 && <p className="text-sm text-slate-500">Showing the first 50 columns. Select fewer fields to inspect other columns.</p>}
      {result.raw !== null && <JsonPreview value={result.raw} />}
      {graphId ? (
        <Suspense fallback={<div className="text-sm text-slate-500">Loading graph viewer…</div>}>
          <GraphVisualization graphId={graphId} refreshKey={result.executionTimeMs} />
        </Suspense>
      ) : null}
    </div>
  )
}

function formatCell(value: unknown) {
  return (typeof value === 'object' && value !== null ? JSON.stringify(value) : String(value ?? '')).slice(0, 2000)
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
