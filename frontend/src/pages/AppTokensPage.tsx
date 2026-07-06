import { CreateAppTokenForm } from '../features/apps/components/CreateAppTokenForm'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Card } from '../shared/components/ui/Card'

export function AppTokensPage() {
  return (
    <div className="space-y-8">
      <PageHeader
        description="Issue scoped bearer tokens for external applications and control what each app can access."
        title="Application tokens"
      />
      <Section title="Create token">
        <Card>
          <CreateAppTokenForm />
        </Card>
      </Section>
    </div>
  )
}
