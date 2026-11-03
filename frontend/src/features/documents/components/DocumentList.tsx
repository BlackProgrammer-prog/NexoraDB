import { PaginatedResults } from '../../../shared/components/ui/PaginatedResults'
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
  if ('documents' in props) return <DocumentListView {...props} />
  return <FetchedDocumentList {...props} />
}

function FetchedDocumentList(props: DocumentListProps) {
  const state = useDocuments(props.collectionName)
  return <DocumentListView {...props} documents={state.data} error={state.error} isLoading={state.isLoading} />
}

function DocumentListView(props: DocumentListProps) {
  const { documents, error, isLoading } = props

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
    <PaginatedResults key={props.collectionName} items={documents}>{(page) => <div className="grid gap-4 lg:grid-cols-2">
      {page.map((document) => (
        <DocumentCard
          document={document}
          key={document.id}
          onDelete={props.onDelete}
          onEdit={props.onEdit}
        />
      ))}
    </div>}</PaginatedResults>
  )
}
