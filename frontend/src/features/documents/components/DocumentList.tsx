import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { useDocuments } from '../hooks/useDocuments'
import { DocumentCard } from './DocumentCard'

interface DocumentListProps {
  collectionName: string
}

export function DocumentList({ collectionName }: DocumentListProps) {
  const { data: documents, error, isLoading } = useDocuments(collectionName)

  if (isLoading) {
    return <LoadingState label="Loading documents" />
  }

  if (error) {
    return <ErrorState message={error.message} />
  }

  if (!documents?.length) {
    return (
      <EmptyState
        description="Documents from the selected collection will be listed here."
        title="No documents found"
      />
    )
  }

  return (
    <div className="grid gap-4 lg:grid-cols-2">
      {documents.map((document) => (
        <DocumentCard document={document} key={document.id} />
      ))}
    </div>
  )
}
