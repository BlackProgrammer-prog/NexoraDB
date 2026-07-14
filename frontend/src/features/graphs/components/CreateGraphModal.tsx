import { useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'
import { graphApi } from '../services/graphApi'

interface CreateGraphModalProps {
  isOpen: boolean
  onClose: () => void
  onCreated: () => void
}

export function CreateGraphModal({ isOpen, onClose, onCreated }: CreateGraphModalProps) {
  const [description, setDescription] = useState('')
  const [isSaving, setIsSaving] = useState(false)
  const [name, setName] = useState('')

  async function handleCreate() {
    if (!name.trim()) {
      return
    }

    setIsSaving(true)
    await graphApi.createGraph({ description: description.trim() || undefined, name: name.trim() })
    setIsSaving(false)
    setName('')
    setDescription('')
    onCreated()
    onClose()
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title="Create graph">
      <div className="space-y-4">
        <Input onChange={(event) => setName(event.target.value)} placeholder="Graph name" value={name} />
        <Input
          onChange={(event) => setDescription(event.target.value)}
          placeholder="Description"
          value={description}
        />
        <Button disabled={isSaving || !name.trim()} onClick={handleCreate}>
          {isSaving ? 'Creating...' : 'Create graph'}
        </Button>
      </div>
    </Modal>
  )
}
