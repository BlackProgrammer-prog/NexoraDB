import { CollectionList } from '../features/collections/components/CollectionList'
import { CreateCollectionModal } from '../features/collections/components/CreateCollectionModal'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Button } from '../shared/components/ui/Button'
import { useDisclosure } from '../shared/hooks/useDisclosure'

export function CollectionsPage() {
  const createModal = useDisclosure()

  return (
    <div className="space-y-8">
      <PageHeader
        actions={<Button onClick={createModal.open}>New collection</Button>}
        description="Collection data is isolated behind feature services and can switch from mocks to FastAPI."
        title="Collections"
      />
      <Section title="Available collections">
        <CollectionList />
      </Section>
      <CreateCollectionModal isOpen={createModal.isOpen} onClose={createModal.close} />
    </div>
  )
}
