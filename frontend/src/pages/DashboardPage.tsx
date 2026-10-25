import { MonitoringPanel } from '../features/monitoring/components/MonitoringPanel'
import { PerformancePanel } from '../features/performance/PerformancePanel'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'

export function DashboardPage() {
  return (
    <div className="space-y-8">
      <PageHeader
        description="Database monitoring and administrator performance controls."
        title="Dashboard"
      />
      <MonitoringPanel />
      <PerformancePanel />
      <Section title="Overview" />
    </div>
  )
}
