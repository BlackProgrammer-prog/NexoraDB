import { Button } from '../../../shared/components/ui/Button'
import { Modal } from '../../../shared/components/ui/Modal'

interface DeleteDocumentModalProps {
  isOpen: boolean
  onClose: () => void
}

export function DeleteDocumentModal({ isOpen, onClose }: DeleteDocumentModalProps) {
  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Delete document">
      <div className="space-y-4">
        <p className="text-sm text-slate-600">Delete confirmation UI will be added later.</p>
        <Button disabled>Delete document</Button>
      </div>
    </Modal>
  )
}
