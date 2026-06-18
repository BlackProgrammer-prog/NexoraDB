export interface ApiResponse<TData> {
  data: TData
  message?: string
}

export interface ApiListResponse<TItem> {
  data: TItem[]
  total: number
}

export type ApiRequestBody = Record<string, unknown>
