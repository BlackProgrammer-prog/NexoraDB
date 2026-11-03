import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { collectionApi } from '../services/collectionApi'

export function useCollections() {
  const loadCollections = useCallback((signal: AbortSignal) => collectionApi.listCollections(signal), [])

  return useAsync(loadCollections)
}
