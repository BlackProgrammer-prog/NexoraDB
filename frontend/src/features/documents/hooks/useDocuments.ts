import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { documentApi } from '../services/documentApi'

export function useDocuments(collectionName: string) {
  const loadDocuments = useCallback(
    () => (collectionName ? documentApi.listDocuments(collectionName) : Promise.resolve([])),
    [collectionName],
  )

  return useAsync(loadDocuments)
}
