import { httpClient } from '../../../api/client/httpClient'
import { documentEndpoints } from '../../../api/endpoints/documentEndpoints'
import type {
  CreateDocumentInput,
  DocumentRecord,
  UpdateDocumentInput,
} from '../types/document.types'

export const documentApi = {
  listDocuments(collectionName: string): Promise<DocumentRecord[]> {
    return httpClient.get<DocumentRecord[]>(documentEndpoints.list(collectionName))
  },

  getDocument(collectionName: string, documentId: string): Promise<DocumentRecord | null> {
    return httpClient.get<DocumentRecord>(
      documentEndpoints.detail(collectionName, documentId),
    )
  },

  createDocument(
    collectionName: string,
    input: CreateDocumentInput,
  ): Promise<DocumentRecord> {
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
    return httpClient.put<DocumentRecord, UpdateDocumentInput>(
      documentEndpoints.update(collectionName, documentId),
      input,
    )
  },

  deleteDocument(collectionName: string, documentId: string): Promise<void> {
    return httpClient.delete<void>(documentEndpoints.delete(collectionName, documentId))
  },
}
