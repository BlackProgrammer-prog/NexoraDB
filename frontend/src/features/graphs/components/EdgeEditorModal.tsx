import { useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { Input } from '../../../shared/components/ui/Input'
import { Modal } from '../../../shared/components/ui/Modal'
import { safeParseJson, stringifyJson } from '../../../shared/utils/json'
import type { GraphEdge, GraphNode } from '../types/graph.types'

interface EdgeEditorModalProps {
  edge: GraphEdge | null
  isOpen: boolean
  nodes: GraphNode[]
  onClose: () => void
  onSave: (input: {
    data: Record<string, unknown>
    label?: string
    source: string
    target: string
  }) => void
}

export function EdgeEditorModal(props: EdgeEditorModalProps) {
  if (!props.isOpen) return null
  const edge = props.edge
  return <EdgeEditorModalForm key={edge?.id} {...props} />
}

function EdgeEditorModalForm({
  edge,
  isOpen,
  nodes,
  onClose,
  onSave,
}: EdgeEditorModalProps) {
  const [data, setData] = useState(() => edge ? stringifyJson(edge.data) : '{}')
  const [jsonError, setJsonError] = useState('')
  const [label, setLabel] = useState(() => edge?.label ?? '')
  const [source, setSource] = useState(() => edge?.source ?? nodes[0]?.id ?? '')
  const [target, setTarget] = useState(() => edge?.target ?? nodes[1]?.id ?? nodes[0]?.id ?? '')

  function handleSave() {
    const parsedData = safeParseJson<Record<string, unknown>>(data)

    if (!parsedData || typeof parsedData !== 'object' || Array.isArray(parsedData)) {
      setJsonError('Data must be a valid JSON object.')
      return
    }

    onSave({ data: parsedData, label: label.trim() || undefined, source, target })
  }

  return (
    <Modal isOpen={isOpen} onClose={onClose} title={edge ? 'Edit edge' : 'Add edge'}>
      <div className="space-y-4">
        <Input onChange={(event) => setLabel(event.target.value)} placeholder="Label" value={label} />
        <select
          className="h-10 w-full rounded-md border border-slate-200 bg-white px-3 text-sm text-slate-900 outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100"
          onChange={(event) => setSource(event.target.value)}
          value={source}
        >
          {nodes.map((node) => (
            <option key={node.id} value={node.id}>
              {node.label}
            </option>
          ))}
        </select>
        <select
          className="h-10 w-full rounded-md border border-slate-200 bg-white px-3 text-sm text-slate-900 outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100"
          onChange={(event) => setTarget(event.target.value)}
          value={target}
        >
          {nodes.map((node) => (
            <option key={node.id} value={node.id}>
              {node.label}
            </option>
          ))}
        </select>
        <textarea
          className="min-h-36 w-full rounded-md border border-slate-200 p-3 font-mono text-sm outline-none focus:border-green-500 focus:ring-2 focus:ring-green-100"
          onChange={(event) => setData(event.target.value)}
          value={data}
        />
        {jsonError ? <p className="text-sm text-red-600">{jsonError}</p> : null}
        <Button disabled={!source || !target} onClick={handleSave}>
          Save edge
        </Button>
      </div>
    </Modal>
  )
}
