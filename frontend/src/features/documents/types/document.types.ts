import type { JsonObject } from '../../../shared/utils/json'

export interface DocumentRecord {
  id: string
  collectionName: string
  data: JsonObject
  createdAt: string
  updatedAt: string
}

export interface CreateDocumentInput {
  data: JsonObject
}

export interface UpdateDocumentInput {
  data: JsonObject
}
