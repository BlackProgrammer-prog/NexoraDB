import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { stringifyJson } from '../../../shared/utils/json'
import type { QueryResult } from '../types/query.types'
import { QueryExecutionStats } from './QueryExecutionStats'

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
        description="Run a mock query to preview tabular rows and raw JSON output."
        title="No query has been executed"
      />
    )
  }

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
    </div>
  )
}
