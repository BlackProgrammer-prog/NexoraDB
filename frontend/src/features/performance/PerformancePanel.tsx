import { useEffect, useState } from 'react'
import { httpClient } from '../../api/client/httpClient'
import { USE_MOCK_API } from '../../api/client/apiConfig'
import { Section } from '../../shared/components/layout/Section'

type Advice = { collection: string; field: string; observations: number; reason: string; query: string }
type Performance = {
  parser_cache: { enabled: boolean; entries: number; hits: number; misses: number; accounted_bytes: number }
  read_cache_admission: boolean | null
  smart_indexes: boolean | null
  native_tuning_available: boolean
  index_advisor: boolean
  advisor_mode: string
  advice: Advice[]
  storage_cache?: { cache_capacity: number; cache_usage: number; cache_hits: number; cache_misses: number; pending_compaction_bytes: number } | null
}
type Toggle = 'parser_cache' | 'read_cache_admission' | 'smart_indexes' | 'index_advisor'

export function PerformancePanel() {
  const [data, setData] = useState<Performance | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  useEffect(() => {
    if (USE_MOCK_API) return
    const controller = new AbortController()
    httpClient.get<Performance>('/performance', { signal: controller.signal })
      .then(setData)
      .catch((reason: unknown) => {
        if (!controller.signal.aborted) setError(reason instanceof Error ? reason.message : 'Unable to load performance settings')
      })
    return () => controller.abort()
  }, [])

  async function refresh() {
    setBusy(true)
    try {
      setData(await httpClient.get<Performance>('/performance'))
      setError('')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Refresh failed')
    } finally { setBusy(false) }
  }

  async function update(options: Partial<Record<Toggle | 'clear_parser_cache', boolean>>) {
    setBusy(true)
    try {
      setData(await httpClient.put<Performance>('/performance', options))
      setError('')
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Update failed; refresh to check current state')
    } finally { setBusy(false) }
  }

  return <Section title="Performance controls" description="Administrator-only runtime settings. Parser cache is process-wide; native controls are per engine. Changes reset on restart.">
    {USE_MOCK_API ? <p>Performance controls require a real backend; mock mode cannot change the engine.</p> : <>
      {error && <p role="alert" className="text-sm text-red-700">{error}</p>}
      <button type="button" disabled={busy} onClick={() => void refresh()} className="rounded border px-3 py-1 disabled:opacity-50">Refresh metrics</button>
      {data && <div className="space-y-4 rounded-xl border border-slate-200 bg-white p-5">
        {([
          ['parser_cache', 'Cache repeated query syntax (never results/parameters)', data.parser_cache.enabled, true],
          ['read_cache_admission', 'Admit newly read data blocks to cache', data.read_cache_admission, data.native_tuning_available],
          ['smart_indexes', 'Use secondary indexes in query planning', data.smart_indexes, data.native_tuning_available],
          ['index_advisor', 'Collect bounded query-shape statistics for index advice', data.index_advisor, true],
        ] as const).map(([key, label, value, available]) => <label key={key} className="flex items-center gap-3 text-sm">
          <input type="checkbox" checked={value === true} disabled={busy || !available}
            onChange={(event) => void update({ [key]: event.target.checked })} />{label}
        </label>)}
        <p className="text-sm text-slate-600">Parser: {data.parser_cache.hits} hits · {data.parser_cache.misses} misses · {data.parser_cache.entries} entries · {(data.parser_cache.accounted_bytes / 1024).toFixed(1)} KiB accounted</p>
        {data.storage_cache && <p className="text-sm text-slate-600">Storage cache: {(data.storage_cache.cache_usage / 1048576).toFixed(1)} / {(data.storage_cache.cache_capacity / 1048576).toFixed(1)} MiB · {data.storage_cache.cache_hits} hits · {data.storage_cache.cache_misses} misses · {(data.storage_cache.pending_compaction_bytes / 1048576).toFixed(1)} MiB pending compaction. Budget changes require restart.</p>}
        <button type="button" disabled={busy} onClick={() => void update({ clear_parser_cache: true })} className="rounded border px-3 py-1 text-sm">Clear parser syntax cache</button>
        <p className="text-xs text-slate-500">Disabling read admission does not remove existing cache hits or metadata caching. Disabling query index selection NEVER disables uniqueness checks or index maintenance.</p>
        <p className="text-xs text-slate-500">Advisor: {data.advisor_mode}. No automatic index creation, remote AI calls or parameter-value collection. Review write/memory cost before running any recommendation in Query.</p>
        {data.advice.length === 0 && <p className="text-sm text-slate-500">No recommendations. Enable the advisor and repeat eligible SELECT predicates at least three times, then refresh.</p>}
        {data.advice.map((item) => <div key={`${item.collection}:${item.field}`} className="rounded border p-3 text-sm">
          <p>{item.collection} · {item.field} · {item.observations} observations</p>
          <p className="text-slate-500">{item.reason}</p>
          <code className="block overflow-x-auto pt-2">{item.query}</code>
        </div>)}
      </div>}
    </>}
  </Section>
}
