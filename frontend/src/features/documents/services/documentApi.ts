import { USE_MOCK_API } from '../../../api/client/apiConfig'
import { httpClient } from '../../../api/client/httpClient'
import { documentEndpoints } from '../../../api/endpoints/documentEndpoints'
import { mockApiAdapter } from '../../../mocks/mockApiAdapter'
import type {
  CreateDocumentInput,
  DocumentRecord,
  UpdateDocumentInput,
} from '../types/document.types'

export const documentApi = {
  listDocuments(collectionName: string): Promise<DocumentRecord[]> {
    if (USE_MOCK_API) {
      return mockApiAdapter.listDocuments(collectionName)
    }

    return httpClient.get<DocumentRecord[]>(documentEndpoints.list(collectionName))
  },

  getDocument(collectionName: string, documentId: string): Promise<DocumentRecord | null> {
    if (USE_MOCK_API) {
      return mockApiAdapter.getDocument(collectionName, documentId)
    }

    return httpClient.get<DocumentRecord>(
      documentEndpoints.detail(collectionName, documentId),
    )
  },

  createDocument(
    collectionName: string,
    input: CreateDocumentInput,
  ): Promise<DocumentRecord> {
    if (USE_MOCK_API) {
      return mockApiAdapter.createDocument(collectionName, input)
    }

    return httpClient.post<DocumentRecord, CreateDocumentInput>(
      documentEndpoints.create(collectionName),
      input,
    )
  },

  updateDocument(
    collectionName: string,
    documentId: string,
    input: UpdateDocumentInput,
  ): Promise<DocumentRecord> {
    if (USE_MOCK_API) {
      return mockApiAdapter.updateDocument(collectionName, documentId, input)
    }

    return httpClient.put<DocumentRecord, UpdateDocumentInput>(
      documentEndpoints.update(collectionName, documentId),
      input,
    )
  },

  deleteDocument(collectionName: string, documentId: string): Promise<void> {
    if (USE_MOCK_API) {
      return mockApiAdapter.deleteDocument(collectionName, documentId)
    }

    return httpClient.delete<void>(documentEndpoints.delete(collectionName, documentId))
  },
}
