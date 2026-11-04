import { QueryEditor } from '../features/query/components/QueryEditor'
import { QueryResultPanel } from '../features/query/components/QueryResultPanel'
import { useQueryRunner } from '../features/query/hooks/useQueryRunner'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Card } from '../shared/components/ui/Card'
import { Button } from '../shared/components/ui/Button'

export function QueryPage() {
  const { error, isIdle, isLoading, result, runQuery, mode, state, submittedQuery, hasMore, loadMore, stop, requestKey } = useQueryRunner()

  return (
    <div className="space-y-8">
      <PageHeader
        description="Run NexoraQL against the native NexoraDB engine and inspect execution time."
        title="Query editor"
      />
      <Card>
        <QueryEditor isRunning={isLoading} onRun={runQuery} error={error} submittedQuery={submittedQuery} />
      </Card>
      <Section title="Result output">
        {requestKey && <p className="mb-3 break-all text-xs text-slate-600">Request key: <code>{requestKey}</code>. Keep this key to reconcile writes after a connection failure.</p>}
        {(isLoading || hasMore) && <div className="mb-3 flex gap-2">
          {hasMore && <Button disabled={isLoading} onClick={() => void loadMore()}>Next server page</Button>}
          <Button variant="secondary" onClick={stop}>{mode === 'cursor' ? 'Close cursor' : 'Stop query'}</Button>
        </div>}
        {state === 'cancelled' && <p className="mb-3 text-sm text-amber-700">Stopped. The preview may be incomplete. Stopping delivery does not roll back writes already committed.</p>}
        <QueryResultPanel
          error={error}
          isIdle={isIdle}
          isLoading={isLoading}
          result={result}
          mode={mode}
        />
      </Section>
    </div>
  )
}
