import { USE_MOCK_API } from '../../../api/client/apiConfig'
import { httpClient } from '../../../api/client/httpClient'
import { queryEndpoints } from '../../../api/endpoints/queryEndpoints'
import { mockApiAdapter } from '../../../mocks/mockApiAdapter'
import type { QueryRequest, QueryResult } from '../types/query.types'

export const queryApi = {
  executeQuery(input: QueryRequest): Promise<QueryResult> {
    if (USE_MOCK_API) {
      return mockApiAdapter.executeQuery(input)
    }

    return httpClient.post<QueryResult, QueryRequest>(queryEndpoints.execute, input)
  },
}
