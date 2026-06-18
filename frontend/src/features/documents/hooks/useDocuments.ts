import { useCallback } from 'react'
import { useAsync } from '../../../shared/hooks/useAsync'
import { documentApi } from '../services/documentApi'

export function useDocuments(collectionName: string) {
  const loadDocuments = useCallback(
    () => documentApi.listDocuments(collectionName),
    [collectionName],
  )

  return useAsync(loadDocuments)
}
