import { useState } from 'react'
import { Button } from './Button'
import { Input } from './Input'

export type CollectionGrants = Record<string, string[]>

export function CollectionGrantsEditor({ value, onChange, label }: {
  value: CollectionGrants
  onChange: (grants: CollectionGrants) => void
  label: string
}) {
  const [collection, setCollection] = useState('')
  const [actions, setActions] = useState<string[]>(['read'])
  return <fieldset className="space-y-2 rounded border p-3">
    <legend className="px-1 text-sm">{label}</legend>
    <Input aria-label={`${label} collection`} placeholder="Exact collection name" value={collection}
      onChange={(event) => setCollection(event.target.value)} maxLength={128} />
    <div className="flex flex-wrap gap-3">
      {['read', 'write', 'manage'].map((action) => <label key={action} className="text-sm">
        <input type="checkbox" checked={actions.includes(action)} onChange={(event) => {
          setActions(event.target.checked ? [...actions, action] : actions.filter((entry) => entry !== action))
        }} /> {action}
      </label>)}
      <Button type="button" disabled={!collection.trim() || !actions.length} onClick={() => {
        onChange({ ...value, [collection.trim()]: actions }); setCollection('')
      }}>Add collection grant</Button>
    </div>
    <p className="text-xs text-slate-500">Manage controls schema and indexes; it does not include read or write. No wildcard access.</p>
    <ul className="space-y-1 text-sm">{Object.entries(value).map(([name, permissions]) => <li key={name}>
      {name}: {permissions.join(', ')}{' '}
      <button type="button" className="underline" aria-label={`Remove ${name} grant`} onClick={() => {
        const next = { ...value }; delete next[name]; onChange(next)
      }}>Remove</button>
    </li>)}</ul>
  </fieldset>
}
