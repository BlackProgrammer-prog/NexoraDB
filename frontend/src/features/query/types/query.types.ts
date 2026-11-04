export interface QueryRequest {
  query: string
  requestKey?: string
}

export interface QueryResult {
  columns: string[]
  rows: Record<string, unknown>[]
  raw: unknown
  executionTimeMs: number
  totalRows?: number
  previewTruncated?: boolean
  statementErrors?: string[]
  pageSequence?: number
}

export type QueryMode = 'buffered' | 'stream' | 'cursor'
export interface QueryDiagnostic {
  message: string
  severity?: 'error' | 'warning'
  line?: number | null
  column?: number | null
}
export interface QueryValidation {
  valid: boolean
  diagnostics: QueryDiagnostic[]
  statementCount: number
  cursorSupported: boolean
  executionStrategy?: 'incremental' | 'prepared'
  requiresIdempotencyKey?: boolean
}
export interface CursorPage {
  statementErrors?: string[]
  cursorId: string | null
  rows: Record<string, unknown>[]
  columns: string[]
  hasMore: boolean
  sequence: number
  totalRows: number
}
