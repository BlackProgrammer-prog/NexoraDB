import { useEffect, useMemo, useState } from 'react'
import { CollectionSidebar } from '../features/collections/components/CollectionSidebar'
import { useCollections } from '../features/collections/hooks/useCollections'
import { DeleteDocumentModal } from '../features/documents/components/DeleteDocumentModal'
import { DocumentEditorModal } from '../features/documents/components/DocumentEditorModal'
import { DocumentList } from '../features/documents/components/DocumentList'
import { useDocuments } from '../features/documents/hooks/useDocuments'
import { documentApi } from '../features/documents/services/documentApi'
import type { DocumentRecord } from '../features/documents/types/document.types'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Button } from '../shared/components/ui/Button'
import { useDisclosure } from '../shared/hooks/useDisclosure'
import type { JsonObject } from '../shared/utils/json'

export function DocumentsPage() {
  const deleteModal = useDisclosure()
  const editorModal = useDisclosure()
  const { data: collections } = useCollections()
  const [documentToDelete, setDocumentToDelete] = useState<DocumentRecord | null>(null)
  const [documentToEdit, setDocumentToEdit] = useState<DocumentRecord | null>(null)
  const [selectedCollectionName, setSelectedCollectionName] = useState('')
  const activeCollectionName = useMemo(
    () => selectedCollectionName || collections?.[0]?.name || '',
    [collections, selectedCollectionName],
  )
  const { data: documents, error, isLoading, refetch } = useDocuments(activeCollectionName)

  useEffect(() => {
    if (!selectedCollectionName && collections?.length) {
      setSelectedCollectionName(collections[0].name)
    }
  }, [collections, selectedCollectionName])

  function openCreateDocument() {
    setDocumentToEdit(null)
    editorModal.open()
  }

  function openEditDocument(document: DocumentRecord) {
    setDocumentToEdit(document)
    editorModal.open()
  }

  function openDeleteDocument(document: DocumentRecord) {
    setDocumentToDelete(document)
    deleteModal.open()
  }

  async function saveDocument(data: JsonObject) {
    if (documentToEdit) {
      await documentApi.updateDocument(activeCollectionName, documentToEdit.id, { data })
    } else {
      await documentApi.createDocument(activeCollectionName, { data })
    }

    editorModal.close()
    await refetch()
  }

  async function deleteDocument() {
    if (!documentToDelete) {
      return
    }

    await documentApi.deleteDocument(activeCollectionName, documentToDelete.id)
    deleteModal.close()
    setDocumentToDelete(null)
    await refetch()
  }

  return (
    <div className="space-y-8">
      <PageHeader
        actions={
          <Button disabled={!activeCollectionName} onClick={openCreateDocument}>
            New document
          </Button>
        }
        description="Create, inspect, update, and delete JSON documents without writing queries."
        title="Documents"
      />
      <div className="grid gap-6 xl:grid-cols-[18rem_1fr]">
        <CollectionSidebar
          onSelect={setSelectedCollectionName}
          selectedCollectionName={activeCollectionName}
        />
        <Section
          description={
            activeCollectionName
              ? `Showing documents for "${activeCollectionName}".`
              : 'Create a collection first to browse documents.'
          }
          title="Selected collection"
        >
          <DocumentList
            collectionName={activeCollectionName}
            documents={documents}
            error={error}
            isLoading={isLoading}
            onDelete={openDeleteDocument}
            onEdit={openEditDocument}
          />
        </Section>
      </div>
      <DocumentEditorModal
        document={documentToEdit}
        isOpen={editorModal.isOpen}
        onClose={editorModal.close}
        onSave={saveDocument}
      />
      <DeleteDocumentModal
        document={documentToDelete}
        isOpen={deleteModal.isOpen}
        onClose={deleteModal.close}
        onConfirm={deleteDocument}
      />
    </div>
  )
}
