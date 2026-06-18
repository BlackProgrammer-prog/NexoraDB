import type {
  Collection,
  CreateCollectionInput,
} from '../features/collections/types/collection.types'
import type {
  CreateDocumentInput,
  DocumentRecord,
  UpdateDocumentInput,
} from '../features/documents/types/document.types'
import { mockCollections } from './mockCollections'
import { mockDocuments } from './mockDocuments'

const latencyMs = 120

function wait() {
  return new Promise((resolve) => {
    window.setTimeout(resolve, latencyMs)
  })
}

let collections = [...mockCollections]
let documents = [...mockDocuments]

export const mockApiAdapter = {
  async listCollections(): Promise<Collection[]> {
    await wait()
    return [...collections]
  },

  async createCollection(input: CreateCollectionInput): Promise<Collection> {
    await wait()
    const timestamp = new Date().toISOString()
    const collection: Collection = {
      createdAt: timestamp,
      documentCount: 0,
      name: input.name,
      sizeBytes: 0,
      updatedAt: timestamp,
    }

    collections = [...collections, collection]
    return collection
  },

  async deleteCollection(collectionName: string): Promise<void> {
    await wait()
    collections = collections.filter((collection) => collection.name !== collectionName)
    documents = documents.filter((document) => document.collectionName !== collectionName)
  },

  async listDocuments(collectionName: string): Promise<DocumentRecord[]> {
    await wait()
    return documents.filter((document) => document.collectionName === collectionName)
  },

  async getDocument(collectionName: string, documentId: string): Promise<DocumentRecord | null> {
    await wait()
    return (
      documents.find(
        (document) =>
          document.collectionName === collectionName && document.id === documentId,
      ) ?? null
    )
  },

  async createDocument(
    collectionName: string,
    input: CreateDocumentInput,
  ): Promise<DocumentRecord> {
    await wait()
    const timestamp = new Date().toISOString()
    const document: DocumentRecord = {
      collectionName,
      createdAt: timestamp,
      data: input.data,
      id: crypto.randomUUID(),
      updatedAt: timestamp,
    }

    documents = [...documents, document]
    return document
  },

  async updateDocument(
    collectionName: string,
    documentId: string,
    input: UpdateDocumentInput,
  ): Promise<DocumentRecord> {
    await wait()
    const timestamp = new Date().toISOString()
    const nextDocuments = documents.map((document) =>
      document.collectionName === collectionName && document.id === documentId
        ? { ...document, data: input.data, updatedAt: timestamp }
        : document,
    )

    documents = nextDocuments
    const updatedDocument = documents.find(
      (document) => document.collectionName === collectionName && document.id === documentId,
    )

    if (!updatedDocument) {
      throw new Error('Document not found')
    }

    return updatedDocument
  },

  async deleteDocument(collectionName: string, documentId: string): Promise<void> {
    await wait()
    documents = documents.filter(
      (document) =>
        document.collectionName !== collectionName || document.id !== documentId,
    )
  },
}
