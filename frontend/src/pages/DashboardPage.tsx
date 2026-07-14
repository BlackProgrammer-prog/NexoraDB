import { MonitoringPanel } from '../features/monitoring/components/MonitoringPanel'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'

export function DashboardPage() {
  return (
    <div className="space-y-8">
      <PageHeader
        description="A lightweight shell for the database admin workflow. Data currently comes from the mock adapter."
        title="Dashboard"
      />
      <MonitoringPanel />
      <Section title="Overview" />
    </div>
  )
}
