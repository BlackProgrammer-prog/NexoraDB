import { appTokenEndpoints } from '../../../api/endpoints/appTokenEndpoints'
import { httpClient } from '../../../api/client/httpClient'
import type {
  AppScopesResponse,
  AppTokenResponse,
  CreateAppTokenInput,
  StoredAppToken,
} from '../types/appToken.types'

export const appTokenApi = {
  listTokens(): Promise<StoredAppToken[]> {
    return httpClient.get<StoredAppToken[]>(appTokenEndpoints.list)
  },
  listScopes(): Promise<AppScopesResponse> {
    return httpClient.get<AppScopesResponse>(appTokenEndpoints.scopes)
  },

  createToken(input: CreateAppTokenInput): Promise<AppTokenResponse> {
    return httpClient.post<AppTokenResponse, CreateAppTokenInput>(
      appTokenEndpoints.create,
      input,
    )
  },

  deleteToken(tokenId: string): Promise<null> {
    return httpClient.delete<null>(appTokenEndpoints.remove(tokenId))
  },
}
