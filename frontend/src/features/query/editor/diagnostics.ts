import type { Diagnostic } from '@codemirror/lint'
import { ApiError } from '../../../api/client/apiError'
import type { QueryDiagnostic } from '../types/query.types'

export function editorDiagnostic(query: string, diagnostic: QueryDiagnostic): Diagnostic {
  const lines = query.split('\n')
  const line = Math.max(0, Math.min(lines.length - 1, (diagnostic.line ?? 1) - 1))
  let from = lines.slice(0, line).reduce((size, text) => size + text.length + 1, 0)
  // Python columns count Unicode code points; CodeMirror offsets use UTF-16.
  from += Array.from(lines[line]).slice(0, Math.max(0, (diagnostic.column ?? 1) - 1)).join('').length
  from = Math.min(from, query.length)
  const width = query.codePointAt(from)! > 0xffff ? 2 : 1
  return { from, to: Math.min(query.length, from + width), message: diagnostic.message,
    severity: diagnostic.severity ?? 'error' }
}

export function executionDiagnostic(error: Error | null): QueryDiagnostic | null {
  if (!(error instanceof ApiError) || !error.details || typeof error.details !== 'object') return null
  const value = (error.details as { queryError?: QueryDiagnostic }).queryError
  return value && typeof value.message === 'string' ? value : null
}
