import { useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'
import { collectionApi } from '../services/collectionApi'

interface CreateCollectionModalProps {
  isOpen: boolean
  onClose: () => void
  onCreated?: () => void
}

export function CreateCollectionModal({
  isOpen,
  onClose,
  onCreated,
}: CreateCollectionModalProps) {
  const [isSaving, setIsSaving] = useState(false)
  const [name, setName] = useState('')

  async function handleCreate() {
    if (!name.trim()) {
      return
    }

    setIsSaving(true)
    await collectionApi.createCollection({ name: name.trim() })
    setIsSaving(false)
    setName('')
    onCreated?.()
    onClose()
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Create collection">
      <div className="space-y-4">
        <Input
          onChange={(event) => setName(event.target.value)}
          placeholder="Collection name"
          value={name}
        />
        <Button disabled={isSaving || !name.trim()} onClick={handleCreate}>
          {isSaving ? 'Creating...' : 'Create collection'}
        </Button>
      </div>
    </Modal>
  )
}
