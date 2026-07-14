import { QueryEditor } from '../features/query/components/QueryEditor'
import { QueryResultPanel } from '../features/query/components/QueryResultPanel'
import { useQueryRunner } from '../features/query/hooks/useQueryRunner'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Card } from '../shared/components/ui/Card'

export function QueryPage() {
  const { error, isIdle, isLoading, result, runQuery } = useQueryRunner()

  return (
    <div className="space-y-8">
      <PageHeader
        description="Run demo database queries against the mock API adapter."
        title="Query editor"
      />
      <Card>
        <QueryEditor isRunning={isLoading} onRun={runQuery} />
      </Card>
      <Section title="Result output">
        <QueryResultPanel
          error={error}
          isIdle={isIdle}
          isLoading={isLoading}
          result={result}
        />
      </Section>
    </div>
  )
}
