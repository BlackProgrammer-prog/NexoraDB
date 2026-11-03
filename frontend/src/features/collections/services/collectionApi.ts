import { collectionEndpoints } from '../../../api/endpoints/collectionEndpoints'
import { httpClient } from '../../../api/client/httpClient'
import type {
  Collection,
  CreateCollectionInput,
  UpdateCollectionInput,
} from '../types/collection.types'

export const collectionApi = {
  listCollections(signal?: AbortSignal): Promise<Collection[]> {
    return httpClient.get<Collection[]>(collectionEndpoints.list, { signal })
  },

  createCollection(input: CreateCollectionInput): Promise<Collection> {
    return httpClient.post<Collection, CreateCollectionInput>(
      collectionEndpoints.create,
      input,
    )
  },

  updateCollection(
    collectionName: string,
    input: UpdateCollectionInput,
  ): Promise<Collection> {
    return httpClient.put<Collection, UpdateCollectionInput>(
      collectionEndpoints.update(collectionName),
      input,
    )
  },

  deleteCollection(collectionName: string): Promise<void> {
    return httpClient.delete<void>(collectionEndpoints.delete(collectionName))
  },
}
