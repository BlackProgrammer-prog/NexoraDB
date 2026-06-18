import { CollectionSidebar } from '../features/collections/components/CollectionSidebar'
import { DocumentEditorModal } from '../features/documents/components/DocumentEditorModal'
import { DocumentList } from '../features/documents/components/DocumentList'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Button } from '../shared/components/ui/Button'
import { useDisclosure } from '../shared/hooks/useDisclosure'

const selectedCollectionName = 'users'

export function DocumentsPage() {
  const editorModal = useDisclosure()

  return (
    <div className="space-y-8">
      <PageHeader
        actions={<Button onClick={editorModal.open}>New document</Button>}
        description="Document components consume hooks only; API details stay in the document service."
        title="Documents"
      />
      <div className="grid gap-6 xl:grid-cols-[18rem_1fr]">
        <CollectionSidebar />
        <Section
          description={`Showing placeholder data for "${selectedCollectionName}".`}
          title="Selected collection"
        >
          <DocumentList collectionName={selectedCollectionName} />
        </Section>
      </div>
      <DocumentEditorModal isOpen={editorModal.isOpen} onClose={editorModal.close} />
    </div>
  )
}
