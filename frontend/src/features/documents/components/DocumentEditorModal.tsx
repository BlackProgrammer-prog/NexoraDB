import { Button } from '../../../shared/components/ui/Button'
import { Modal } from '../../../shared/components/ui/Modal'

interface DocumentEditorModalProps {
  isOpen: boolean
  onClose: () => void
}

export function DocumentEditorModal({ isOpen, onClose }: DocumentEditorModalProps) {
  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Document editor">
      <div className="space-y-4">
        <div className="min-h-40 rounded-md border border-dashed border-slate-300 bg-slate-50" />
        <Button disabled>Save document</Button>
      </div>
    </Modal>
  )
}
