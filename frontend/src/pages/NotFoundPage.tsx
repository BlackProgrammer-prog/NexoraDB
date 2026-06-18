import { PageHeader } from '../shared/components/layout/PageHeader'
import { EmptyState } from '../shared/components/ui/EmptyState'

export function NotFoundPage() {
  return (
    <div className="space-y-8">
      <PageHeader title="Page not found" />
      <EmptyState
        description="The requested admin panel page does not exist."
        title="Nothing here"
      />
    </div>
  )
}
