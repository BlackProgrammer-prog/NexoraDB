import type { FormEvent } from 'react'
import { useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { QueryExamples } from './QueryExamples'

interface QueryEditorProps {
  isRunning: boolean
  onRun: (query: string) => void
}

export function QueryEditor({ isRunning, onRun }: QueryEditorProps) {
  const [query, setQuery] = useState('SHOW COLLECTIONS')

  function handleSubmit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    onRun(query)
  }

  return (
    <form className="space-y-4" onSubmit={handleSubmit}>
      <div className="space-y-2">
        <label className="text-sm font-medium text-slate-700" htmlFor="query-editor">
          Query
        </label>
        <textarea
          className="min-h-44 w-full resize-y rounded-md border border-slate-200 bg-white p-4 font-mono text-sm text-slate-900 outline-none transition placeholder:text-slate-400 focus:border-green-500 focus:ring-2 focus:ring-green-100"
          id="query-editor"
          onChange={(event) => setQuery(event.target.value)}
          placeholder="SHOW COLLECTIONS"
          value={query}
        />
      </div>
      <div className="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <QueryExamples onSelect={setQuery} />
        <Button className="lg:w-32" disabled={isRunning} type="submit">
          {isRunning ? 'Running...' : 'Run Query'}
        </Button>
      </div>
    </form>
  )
}
