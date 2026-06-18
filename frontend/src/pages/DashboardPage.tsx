import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Card } from '../shared/components/ui/Card'

const stats = [
  { label: 'Collections', value: '3' },
  { label: 'Documents', value: '187' },
  { label: 'API mode', value: 'Mock' },
]

export function DashboardPage() {
  return (
    <div className="space-y-8">
      <PageHeader
        description="A lightweight shell for the database admin workflow. Data currently comes from the mock adapter."
        title="Dashboard"
      />
      <Section title="Overview">
        <div className="grid gap-4 sm:grid-cols-3">
          {stats.map((stat) => (
            <Card key={stat.label}>
              <p className="text-sm text-slate-500">{stat.label}</p>
              <p className="mt-2 text-2xl font-semibold text-slate-950">{stat.value}</p>
            </Card>
          ))}
        </div>
      </Section>
    </div>
  )
}
