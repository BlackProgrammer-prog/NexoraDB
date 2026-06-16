export interface Collection {
  name: string
  documentCount: number
  sizeBytes: number
  createdAt: string
  updatedAt: string
}

export interface CreateCollectionInput {
  name: string
}
