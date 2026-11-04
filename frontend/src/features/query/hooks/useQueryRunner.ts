import { useEffect, useRef, useState } from 'react'
import { queryApi } from '../services/queryApi'
import { ResultPreview } from '../services/resultPreview'
import type { CursorPage, QueryMode, QueryResult } from '../types/query.types'

type QueryRunState = 'idle' | 'loading' | 'success' | 'error' | 'cancelled'
export function useQueryRunner() {
  const [error, setError] = useState<Error | null>(null)
  const [result, setResult] = useState<QueryResult | null>(null)
  const [state, setState] = useState<QueryRunState>('idle')
  const [mode, setMode] = useState<QueryMode>('buffered')
  const [submittedQuery, setSubmittedQuery] = useState('')
  const [hasMore, setHasMore] = useState(false)
  const [requestKey, setRequestKey] = useState<string | null>(null)
  const active = useRef<AbortController | null>(null)
  const cursor = useRef<CursorPage | null>(null)
  const fetchingPage = useRef<AbortController | null>(null)
  function releaseCursor() {
    const id = cursor.current?.cursorId
    cursor.current = null
    if (id) void queryApi.closeCursor(id).catch(() => {}) // Server TTL is the fallback after session loss.
  }
  useEffect(() => () => { active.current?.abort(); releaseCursor() }, [])
  function pageResult(page: CursorPage): QueryResult {
    return { columns: page.columns, rows: page.rows, totalRows: page.totalRows, raw: null, executionTimeMs: 0, pageSequence: page.sequence, statementErrors: page.statementErrors }
  }
  async function runQuery(query: string, nextMode: QueryMode = 'buffered') {
    active.current?.abort()
    releaseCursor()
    const controller = new AbortController()
    active.current = controller
    setError(null); setResult(null); setHasMore(false)
    setMode(nextMode); setSubmittedQuery(query); setState('loading')
    const key = nextMode === 'buffered' ? undefined : crypto.randomUUID()
    setRequestKey(key ?? null)
    try {
      if (nextMode === 'buffered') {
        const response = await queryApi.executeQuery({ query }, controller.signal)
        if (!controller.signal.aborted) {
          setResult(response)
          if (response.statementErrors?.length) {
            setError(new Error(`One or more statements failed. Earlier writes may have committed. ${response.statementErrors.join('; ')}`))
            setState('error')
            return
          }
        }
      } else if (nextMode === 'cursor') {
        const page = await queryApi.openCursor({ query, requestKey: key }, controller.signal)
        if (controller.signal.aborted) {
          if (page.cursorId) void queryApi.closeCursor(page.cursorId).catch(() => {})
          return
        }
        cursor.current = page
        setResult(pageResult(page)); setHasMore(page.hasMore)
        if (page.statementErrors?.length) throw new Error(`Statement failed; earlier writes may have committed. ${page.statementErrors.join('; ')}`)
      } else {
        const preview = new ResultPreview()
        await queryApi.streamQuery({ query, requestKey: key }, controller.signal, async (batch) => {
          if (controller.signal.aborted) return
          preview.add(batch)
          const rows = preview.snapshot()
          const columns = new Set<string>()
          for (const row of rows) {
            for (const key in row) {
              if (columns.size >= 1000) break
              if (Object.hasOwn(row, key)) columns.add(key)
            }
            if (columns.size >= 1000) break
          }
          setResult({ rows, columns: [...columns], raw: null, executionTimeMs: 0, totalRows: preview.total, previewTruncated: preview.total > rows.length })
          // Apply browser backpressure rather than queueing a render per record.
          await new Promise<void>((resolve) => window.setTimeout(resolve, 16))
        })
        if (preview.total === 0 && !controller.signal.aborted) setResult({ columns: [], rows: [], raw: null, executionTimeMs: 0, totalRows: 0 })
      }
      if (!controller.signal.aborted) setState('success')
    } catch (reason) {
      if (!controller.signal.aborted) {
        releaseCursor(); setHasMore(false)
        setError(reason instanceof Error ? reason : new Error('Unexpected query error'))
        setState('error')
      }
    }
  }
  async function loadMore() {
    const page = cursor.current
    if (!page?.cursorId || active.current?.signal.aborted || (fetchingPage.current && !fetchingPage.current.signal.aborted)) return
    active.current?.abort()
    const controller = new AbortController()
    active.current = controller
    fetchingPage.current = controller
    setState('loading'); setError(null)
    try {
      const next = await queryApi.nextPage(page.cursorId, page.sequence + 1, controller.signal)
      if (!controller.signal.aborted) {
        cursor.current = next
        setResult((current) => next.rows.length || !current ? pageResult(next) : { ...current, totalRows: next.totalRows })
        setHasMore(next.hasMore); setState('success')
        if (next.statementErrors?.length) throw new Error(`Statement failed; earlier writes may have committed. ${next.statementErrors.join('; ')}`)
      }
    } catch (reason) {
      if (!controller.signal.aborted) {
        releaseCursor(); setHasMore(false)
        setError(new Error(`${reason instanceof Error ? reason.message : 'Cursor page failed'}. The cursor may have advanced. Reconcile the request key before retrying writes.`))
        setState('error')
      }
    } finally { if (fetchingPage.current === controller) fetchingPage.current = null }
  }
  function stop() {
    active.current?.abort()
    releaseCursor(); setHasMore(false); setState('cancelled')
  }
  return { error, isIdle: state === 'idle', isLoading: state === 'loading', result, runQuery,
    mode, state, submittedQuery, hasMore, loadMore, stop, requestKey }
}
