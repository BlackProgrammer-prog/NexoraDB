import { useState, type FormEvent } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { QueryExamples } from './QueryExamples'
import { CodeEditor } from '../editor/CodeEditor'
import { executionDiagnostic } from '../editor/diagnostics'
import { useQueryDiagnostics } from '../hooks/useQueryDiagnostics'
import type { QueryMode } from '../types/query.types'

interface QueryEditorProps {
  isRunning: boolean
  onRun: (query: string, mode?: QueryMode) => void
  error?: Error | null
  submittedQuery?: string
}

export function QueryEditor({ isRunning, onRun, error = null, submittedQuery }: QueryEditorProps) {
  const [query, setQuery] = useState('SHOW COLLECTIONS;')
  const [mode, setMode] = useState<QueryMode>('buffered')
  const validation = useQueryDiagnostics(query)
  const execution = query === submittedQuery ? executionDiagnostic(error) ?? (error ? { message: error.message } : null) : null
  const diagnostics = [...(validation.response?.diagnostics ?? []), ...(execution ? [execution] : [])]
  const incompatible = mode !== 'buffered' && validation.response?.valid && !validation.response.cursorSupported
  const disabled = isRunning || !query.trim() || validation.response?.valid === false || Boolean(incompatible)
  function run(current = query) { if (!disabled) onRun(current, mode) }
  function handleSubmit(event: FormEvent<HTMLFormElement>) { event.preventDefault(); run() }

  return <form className="space-y-4" onSubmit={handleSubmit}>
    <div className="space-y-2">
      <span className="text-sm font-medium text-slate-700">Query</span>
      <CodeEditor value={query} onChange={setQuery} onRun={run} diagnostics={diagnostics} />
      <p className="text-xs text-slate-500">Enter adds a line. Ctrl/Cmd+Enter runs the query. Tab indents; Escape then Tab leaves the editor. Maximum 200,000 characters.</p>
      <div aria-live="polite" className="text-sm">
        {diagnostics.map((item, index) => <p className="text-red-700" key={index}>
          {item.line ? `Line ${item.line}${item.column ? `, column ${item.column}` : ''}: ` : ''}{item.message}
        </p>)}
        {!query.trim() ? null : validation.unavailable ? <p className="text-amber-700">Diagnostics unavailable: {validation.unavailable}</p>
          : !validation.response ? <p className="text-slate-500">Checking query...</p>
            : validation.response.valid ? <p className="text-green-700">Syntax and semantic checks passed. Database schema and execution are checked when run.</p> : null}
      </div>
    </div>
    <label className="flex items-center gap-3 text-sm">Results mode
      <select aria-label="Results mode" className="rounded border border-slate-300 bg-white p-2" value={mode} onChange={(event) => setMode(event.target.value as QueryMode)} disabled={isRunning}>
        <option value="buffered">Buffered (all statements)</option>
        <option value="stream">Streaming preview</option>
        <option value="cursor">Server cursor (100 rows per page)</option>
      </select>
    </label>
    {mode !== 'buffered' && <p className="text-xs text-slate-600">Supports scripts and up to eight chained LOOKUP JOIN targets. Each SELECT and its JOINs share a snapshot; separate statements use separate snapshots. Writes finish before delivery and return execution receipts. Streaming previews retain up to 1,000 records within 8 MiB. Cursors expire after 30 seconds idle or 120 seconds total.</p>}
    {mode !== 'buffered' && validation.response?.requiresIdempotencyKey && <p className="text-sm text-amber-700">This program can change data. Results are prepared before delivery. Each run uses a new request key; after a connection failure, reconcile the outcome before running it again.</p>}
    {incompatible && <p className="text-sm text-amber-700">This program exceeds the cursor execution contract.</p>}
    <div className="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
      <QueryExamples onSelect={setQuery} />
      <Button className="lg:w-32" disabled={disabled} type="submit">{isRunning ? 'Running...' : 'Run Query'}</Button>
    </div>
  </form>
}
