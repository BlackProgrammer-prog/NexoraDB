import { useState } from 'react'
import { CreateAppTokenForm } from '../features/apps/components/CreateAppTokenForm'
import { AppTokenList } from '../features/apps/components/AppTokenList'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Card } from '../shared/components/ui/Card'

export function AppTokensPage() {
  const [refreshKey, setRefreshKey] = useState(0)

  return (
    <div className="min-w-0 space-y-5 sm:space-y-8">
      <PageHeader
        description="Issue scoped bearer tokens for external applications and control what each app can access."
        title="Application tokens"
      />
      <Section title="Create token">
        <Card className="min-w-0 overflow-hidden">
          <CreateAppTokenForm onCreated={() => setRefreshKey((value) => value + 1)} />
        </Card>
      </Section>
      <Section title="Issued tokens">
        <Card className="min-w-0 overflow-hidden">
          <AppTokenList refreshKey={refreshKey} />
        </Card>
      </Section>
    </div>
  )
}
