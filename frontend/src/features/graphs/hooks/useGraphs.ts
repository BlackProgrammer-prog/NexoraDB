import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { graphApi } from '../services/graphApi'

export function useGraphs() {
  const loadGraphs = useCallback(() => graphApi.listGraphs(), [])

  return useAsync(loadGraphs)
}
