import { Button } from '../../../shared/components/ui/Button'
import { Modal } from '../../../shared/components/ui/Modal'
import type { Graph } from '../types/graph.types'

interface DeleteGraphModalProps {
  graph: Graph | null
  isOpen: boolean
  isDeleting: boolean
  onClose: () => void
  onConfirm: () => void
}

export function DeleteGraphModal({
  graph,
  isDeleting,
  isOpen,
  onClose,
  onConfirm,
}: DeleteGraphModalProps) {
  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Delete graph">
      <div className="space-y-4">
        <p className="text-sm text-slate-600">
          Delete {graph ? `"${graph.name}"` : 'this graph'} and all of its nodes and edges?
        </p>
        <Button disabled={isDeleting || !graph} onClick={onConfirm}>
          {isDeleting ? 'Deleting...' : 'Delete graph'}
        </Button>
      </div>
    </Modal>
  )
}
