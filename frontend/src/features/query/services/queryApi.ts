import { USE_MOCK_API } from '../../../api/client/apiConfig'
import { httpClient } from '../../../api/client/httpClient'
import { queryEndpoints } from '../../../api/endpoints/queryEndpoints'
import { mockApiAdapter } from '../../../mocks/mockApiAdapter'
import type { CursorPage, QueryRequest, QueryResult, QueryValidation } from '../types/query.types'
import { streamQuery } from './queryStream'

export const queryApi = {
  validate(input: QueryRequest, signal?: AbortSignal) {
    return httpClient.post<QueryValidation, QueryRequest>('/query/validate', input, { signal })
  },
  streamQuery,
  openCursor(input: QueryRequest, signal?: AbortSignal) {
    return httpClient.post<CursorPage, QueryRequest & { pageSize: number }>('/query/cursors', { ...input, pageSize: 100 },
      { signal, headers: { 'Idempotency-Key': input.requestKey ?? crypto.randomUUID() } })
  },
  nextPage(id: string, sequence: number, signal?: AbortSignal) {
    return httpClient.post<CursorPage, { sequence: number; pageSize: number }>(`/query/cursors/${encodeURIComponent(id)}/next`, { sequence, pageSize: 100 }, { signal })
  },
  closeCursor(id: string) {
    return httpClient.delete<null>(`/query/cursors/${encodeURIComponent(id)}`)
  },
  executeQuery(input: QueryRequest, signal?: AbortSignal): Promise<QueryResult> {
    if (USE_MOCK_API) {
      return mockApiAdapter.executeQuery(input)
    }

    return httpClient.post<QueryResult, QueryRequest>(queryEndpoints.execute, input, { signal })
  },
}
