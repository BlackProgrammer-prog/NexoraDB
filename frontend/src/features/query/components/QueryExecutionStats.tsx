interface QueryExecutionStatsProps {
  executionTimeMs: number
}

export function QueryExecutionStats({ executionTimeMs }: QueryExecutionStatsProps) {
  return (
    <p className="rounded-md bg-green-50 px-3 py-2 text-sm font-medium text-green-700">
      Executed in {executionTimeMs}ms
    </p>
  )
}
