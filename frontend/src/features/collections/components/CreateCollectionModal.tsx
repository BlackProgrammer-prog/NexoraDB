import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'

interface CreateCollectionModalProps {
  isOpen: boolean
  onClose: () => void
}

export function CreateCollectionModal({ isOpen, onClose }: CreateCollectionModalProps) {
  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Create collection">
      <div className="space-y-4">
        <Input disabled placeholder="Collection name" />
        <Button disabled>Create collection</Button>
      </div>
    </Modal>
  )
}
