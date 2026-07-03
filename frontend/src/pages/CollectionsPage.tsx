import { CollectionList } from '../features/collections/components/CollectionList'
import { CreateCollectionModal } from '../features/collections/components/CreateCollectionModal'
import { UpdateCollectionModal } from '../features/collections/components/UpdateCollectionModal'
import { useCollections } from '../features/collections/hooks/useCollections'
import { collectionApi } from '../features/collections/services/collectionApi'
import type { Collection } from '../features/collections/types/collection.types'
import { DocumentList } from '../features/documents/components/DocumentList'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Button } from '../shared/components/ui/Button'
import { useDisclosure } from '../shared/hooks/useDisclosure'
import { useState } from 'react'

export function CollectionsPage() {
  const createModal = useDisclosure()
  const updateModal = useDisclosure()
  const { data: collections, error, isLoading, refetch } = useCollections()
  const [collectionToEdit, setCollectionToEdit] = useState<Collection | null>(null)
  const [selectedCollectionName, setSelectedCollectionName] = useState('users')

  async function deleteCollection(collection: Collection) {
    await collectionApi.deleteCollection(collection.name)
    if (selectedCollectionName === collection.name) {
      setSelectedCollectionName(collections?.[0]?.name ?? '')
    }
    await refetch()
  }

  function editCollection(collection: Collection) {
    setCollectionToEdit(collection)
    updateModal.open()
  }

  return (
    <div className="space-y-8">
      <PageHeader
        actions={<Button onClick={createModal.open}>New collection</Button>}
        description="Collection data is isolated behind feature services and can switch from mocks to FastAPI."
        title="Collections"
      />
      <Section title="Available collections">
        <CollectionList
          collections={collections}
          error={error}
          isLoading={isLoading}
          onDelete={deleteCollection}
          onEdit={editCollection}
          onView={(collection) => setSelectedCollectionName(collection.name)}
        />
      </Section>
      {selectedCollectionName ? (
        <Section
          description={`Viewing documents for "${selectedCollectionName}".`}
          title="Collection documents"
        >
          <DocumentList collectionName={selectedCollectionName} />
        </Section>
      ) : null}
      <CreateCollectionModal
        isOpen={createModal.isOpen}
        onClose={createModal.close}
        onCreated={refetch}
      />
      <UpdateCollectionModal
        collection={collectionToEdit}
        isOpen={updateModal.isOpen}
        onClose={updateModal.close}
        onUpdated={refetch}
      />
    </div>
  )
}
