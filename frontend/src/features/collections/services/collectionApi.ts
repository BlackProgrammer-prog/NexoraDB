import { collectionEndpoints } from '../../../api/endpoints/collectionEndpoints'
import { USE_MOCK_API } from '../../../api/client/apiConfig'
import { httpClient } from '../../../api/client/httpClient'
import { mockApiAdapter } from '../../../mocks/mockApiAdapter'
import type {
  Collection,
  CreateCollectionInput,
  UpdateCollectionInput,
} from '../types/collection.types'

export const collectionApi = {
  listCollections(): Promise<Collection[]> {
    if (USE_MOCK_API) {
      return mockApiAdapter.listCollections()
    }

    return httpClient.get<Collection[]>(collectionEndpoints.list)
  },

  createCollection(input: CreateCollectionInput): Promise<Collection> {
    if (USE_MOCK_API) {
      return mockApiAdapter.createCollection(input)
    }

    return httpClient.post<Collection, CreateCollectionInput>(
      collectionEndpoints.create,
      input,
    )
  },

  updateCollection(
    collectionName: string,
    input: UpdateCollectionInput,
  ): Promise<Collection> {
    if (USE_MOCK_API) {
      return mockApiAdapter.updateCollection(collectionName, input)
    }

    return httpClient.put<Collection, UpdateCollectionInput>(
      collectionEndpoints.update(collectionName),
      input,
    )
  },

  deleteCollection(collectionName: string): Promise<void> {
    if (USE_MOCK_API) {
      return mockApiAdapter.deleteCollection(collectionName)
    }

    return httpClient.delete<void>(collectionEndpoints.delete(collectionName))
  },
}
