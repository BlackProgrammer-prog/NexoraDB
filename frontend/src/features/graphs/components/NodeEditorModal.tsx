import { useEffect, useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'
import { safeParseJson, stringifyJson } from '../../../shared/utils/json'
import type { GraphNode } from '../types/graph.types'

interface NodeEditorModalProps {
  isOpen: boolean
  node: GraphNode | null
  onClose: () => void
  onSave: (input: { label: string; type?: string; data: Record<string, unknown> }) => void
}

export function NodeEditorModal({ isOpen, node, onClose, onSave }: NodeEditorModalProps) {
  const [data, setData] = useState('{}')
  const [jsonError, setJsonError] = useState('')
  const [label, setLabel] = useState('')
  const [type, setType] = useState('')

  useEffect(() => {
    setLabel(node?.label ?? '')
    setType(node?.type ?? '')
    setData(node ? stringifyJson(node.data) : '{}')
    setJsonError('')
  }, [node, isOpen])

  function handleSave() {
    const parsedData = safeParseJson<Record<string, unknown>>(data)

    if (!parsedData || Array.isArray(parsedData)) {
      setJsonError('Data must be a valid JSON object.')
      return
    }

    onSave({ data: parsedData, label: label.trim(), type: type.trim() || undefined })
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title={node ? 'Edit node' : 'Add node'}>
      <div className="space-y-4">
        <Input onChange={(event) => setLabel(event.target.value)} placeholder="Label" value={label} />
        <Input onChange={(event) => setType(event.target.value)} placeholder="Type" value={type} />
        <textarea
          className="min-h-36 w-full rounded-md border border-slate-200 p-3 font-mono text-sm outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100"
          onChange={(event) => setData(event.target.value)}
          value={data}
        />
        {jsonError ? <p className="text-sm text-red-600">{jsonError}</p> : null}
        <Button disabled={!label.trim()} onClick={handleSave}>
          Save node
        </Button>
      </div>
    </Modal>
  )
}
