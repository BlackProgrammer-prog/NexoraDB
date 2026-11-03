import { useEffect, useRef, useState } from 'react'
import { queryApi } from '../services/queryApi'
import type { QueryResult } from '../types/query.types'

type QueryRunState = 'idle' | 'loading' | 'success' | 'error'
export function useQueryRunner() {
  const [error, setError] = useState<Error | null>(null)
  const [result, setResult] = useState<QueryResult | null>(null)
  const [state, setState] = useState<QueryRunState>('idle')
  const active = useRef<AbortController | null>(null)
  useEffect(() => () => active.current?.abort(), [])
  async function runQuery(query: string) {
    active.current?.abort()
    const controller = new AbortController()
    active.current = controller
    setError(null)
    setResult(null)
    setState('loading')
    try {
      const response = await queryApi.executeQuery({ query }, controller.signal)
      if (!controller.signal.aborted) { setResult(response); setState('success') }
    } catch (reason) {
      if (!controller.signal.aborted) {
        setError(reason instanceof Error ? reason : new Error('Unexpected query error'))
        setState('error')
      }
    }
  }
  return { error, isIdle: state === 'idle', isLoading: state === 'loading', result, runQuery }
}
