import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { graphApi } from '../services/graphApi'

export function useGraphs() {
  const loadGraphs = useCallback((signal: AbortSignal) => graphApi.listGraphs(signal), [])

  return useAsync(loadGraphs)
}
