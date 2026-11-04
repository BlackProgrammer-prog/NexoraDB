import { useEffect, useState } from 'react'
import { queryApi } from '../services/queryApi'
import type { QueryValidation } from '../types/query.types'

export function useQueryDiagnostics(query: string) {
  const [state, setState] = useState<{ query: string; response: QueryValidation | null; unavailable: string | null }>({ query: '', response: null, unavailable: null })
  useEffect(() => {
    const controller = new AbortController()
    const timer = window.setTimeout(() => {
      if (!query.trim()) return
      void queryApi.validate({ query }, controller.signal).then(
        (response) => { if (!controller.signal.aborted) setState({ query, response, unavailable: null }) },
        (error: unknown) => { if (!controller.signal.aborted) setState({ query, response: null, unavailable: error instanceof Error ? error.message : 'Validation failed' }) },
      )
    }, 400)
    return () => { window.clearTimeout(timer); controller.abort() }
  }, [query])
  return state.query === query ? state : { query, response: null, unavailable: null }
}
