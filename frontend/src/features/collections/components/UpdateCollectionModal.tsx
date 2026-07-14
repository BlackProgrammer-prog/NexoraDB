import { useEffect, useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'
import { collectionApi } from '../services/collectionApi'
import type { Collection } from '../types/collection.types'

interface UpdateCollectionModalProps {
  collection: Collection | null
  isOpen: boolean
  onClose: () => void
  onUpdated: () => void
}

export function UpdateCollectionModal({
  collection,
  isOpen,
  onClose,
  onUpdated,
}: UpdateCollectionModalProps) {
  const [isSaving, setIsSaving] = useState(false)
  const [name, setName] = useState('')

  useEffect(() => {
    setName(collection?.name ?? '')
  }, [collection, isOpen])

  async function handleUpdate() {
    if (!collection || !name.trim()) {
      return
    }

    setIsSaving(true)
    await collectionApi.updateCollection(collection.name, { name: name.trim() })
    setIsSaving(false)
    onUpdated()
    onClose()
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Update collection">
      <div className="space-y-4">
        <Input
          onChange={(event) => setName(event.target.value)}
          placeholder="Collection name"
          value={name}
        />
        <Button disabled={isSaving || !collection || !name.trim()} onClick={handleUpdate}>
          {isSaving ? 'Saving...' : 'Save collection'}
        </Button>
      </div>
    </Modal>
  )
}
