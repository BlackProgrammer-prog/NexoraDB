import { useEffect, useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Modal } from '../../../shared/components/ui/Modal'
import type { JsonObject } from '../../../shared/utils/json'
import { safeParseJson, stringifyJson } from '../../../shared/utils/json'
import type { DocumentRecord } from '../types/document.types'

interface DocumentEditorModalProps {
  document?: DocumentRecord | null
  isOpen: boolean
  onClose: () => void
  onSave: (data: JsonObject) => void
}

export function DocumentEditorModal({
  document,
  isOpen,
  onClose,
  onSave,
}: DocumentEditorModalProps) {
  const [jsonError, setJsonError] = useState('')
  const [value, setValue] = useState('{}')

  useEffect(() => {
    setValue(document ? stringifyJson(document.data) : '{}')
    setJsonError('')
  }, [document, isOpen])

  function handleSave() {
    const parsed = safeParseJson<JsonObject>(value)

    if (!parsed || Array.isArray(parsed)) {
      setJsonError('Document JSON must be a valid object.')
      return
    }

    onSave(parsed)
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title={document ? 'Edit document' : 'Create document'}>
      <div className="space-y-4">
        <textarea
          className="min-h-56 w-full rounded-md border border-slate-200 p-3 font-mono text-sm outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100"
          onChange={(event) => setValue(event.target.value)}
          value={value}
        />
        {jsonError ? <p className="text-sm text-red-600">{jsonError}</p> : null}
        <Button onClick={handleSave}>Save document</Button>
      </div>
    </Modal>
  )
}
