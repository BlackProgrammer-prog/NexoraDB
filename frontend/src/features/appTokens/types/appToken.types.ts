export interface CreateAppTokenInput {
  appId: string
  appName?: string
  scopes: string[]
  expiresInSeconds?: number
}

export interface AppTokenResponse {
  appId: string
  appName: string
  token: string
  tokenType: 'bearer'
  expiresAt: number | null
  scopes: string[]
}

export interface AppScopesResponse {
  scopes: string[]
}
