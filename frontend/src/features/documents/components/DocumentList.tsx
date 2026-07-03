import { EmptyState } from '../../../shared/components/ui/EmptyState'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { useDocuments } from '../hooks/useDocuments'
import type { DocumentRecord } from '../types/document.types'
import { DocumentCard } from './DocumentCard'

interface DocumentListProps {
  collectionName: string
  documents?: DocumentRecord[] | null
  error?: Error | null
  isLoading?: boolean
  onDelete?: (document: DocumentRecord) => void
  onEdit?: (document: DocumentRecord) => void
}

export function DocumentList(props: DocumentListProps) {
  const internalState = useDocuments(props.collectionName)
  const documents = props.documents ?? internalState.data
  const error = props.error ?? internalState.error
  const isLoading = props.isLoading ?? internalState.isLoading

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
        <DocumentCard
          document={document}
          key={document.id}
          onDelete={props.onDelete}
          onEdit={props.onEdit}
        />
      ))}
    </div>
  )
}
