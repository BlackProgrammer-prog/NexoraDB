export interface QueryRequest {
  query: string
}

export interface QueryResult {
  columns: string[]
  rows: Record<string, unknown>[]
  raw: unknown
  executionTimeMs: number
}
