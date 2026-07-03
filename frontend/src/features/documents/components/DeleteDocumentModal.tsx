import { Button } from '../../../shared/components/ui/Button'
import { Modal } from '../../../shared/components/ui/Modal'
import type { DocumentRecord } from '../types/document.types'

interface DeleteDocumentModalProps {
  document: DocumentRecord | null
  isOpen: boolean
  onClose: () => void
  onConfirm: () => void
}

export function DeleteDocumentModal({
  document,
  isOpen,
  onClose,
  onConfirm,
}: DeleteDocumentModalProps) {
  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Delete document">
      <div className="space-y-4">
        <p className="text-sm text-slate-600">
          Delete document {document ? `"${document.id}"` : ''}?
        </p>
        <Button disabled={!document} onClick={onConfirm}>
          Delete document
        </Button>
      </div>
    </Modal>
  )
}
