import { httpClient } from '../../../api/client/httpClient'
import { appTokenEndpoints } from '../../../api/endpoints/appTokenEndpoints'
import type {
  AppScopesResponse,
  AppTokenResponse,
  CreateAppTokenInput,
} from '../types/appToken.types'

export const appTokenApi = {
  listScopes(): Promise<AppScopesResponse> {
    return httpClient.get<AppScopesResponse>(appTokenEndpoints.scopes)
  },

  createToken(input: CreateAppTokenInput): Promise<AppTokenResponse> {
    return httpClient.post<AppTokenResponse, CreateAppTokenInput>(
      appTokenEndpoints.create,
      input,
    )
  },
}
