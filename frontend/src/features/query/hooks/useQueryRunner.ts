import { useState } from 'react'
import { queryApi } from '../services/queryApi'
import type { QueryResult } from '../types/query.types'

type QueryRunState = 'idle' | 'loading' | 'success' | 'error'

export function useQueryRunner() {
  const [error, setError] = useState<Error | null>(null)
  const [result, setResult] = useState<QueryResult | null>(null)
  const [state, setState] = useState<QueryRunState>('idle')

  async function runQuery(query: string) {
    setError(null)
    setState('loading')

    try {
      const response = await queryApi.executeQuery({ query })
      setResult(response)
      setState('success')
    } catch (runError) {
      setResult(null)
      setError(runError instanceof Error ? runError : new Error('Unexpected query error'))
      setState('error')
    }
  }

  return {
    error,
    isIdle: state === 'idle',
    isLoading: state === 'loading',
    result,
    runQuery,
  }
}
