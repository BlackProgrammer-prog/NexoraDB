import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { collectionApi } from '../services/collectionApi'

export function useCollections() {
  const loadCollections = useCallback(() => collectionApi.listCollections(), [])

  return useAsync(loadCollections)
}
