import { authEndpoints } from '../../../api/endpoints/authEndpoints'
import { httpClient } from '../../../api/client/httpClient'
import type {
  AdminUser,
  AuthResponse,
  LoginInput,
  RegisterInput,
  SetupStateResponse,
} from '../types/auth.types'

export const authApi = {
  getSetupState(): Promise<SetupStateResponse> {
    return httpClient.get<SetupStateResponse>(authEndpoints.setupState)
  },

  register(input: RegisterInput): Promise<AdminUser> {
    return httpClient.post<AdminUser, RegisterInput>(authEndpoints.register, input)
  },

  login(input: LoginInput): Promise<AuthResponse> {
    return httpClient.post<AuthResponse, LoginInput>(authEndpoints.login, input)
  },

  me(accessToken: string): Promise<AdminUser> {
    return httpClient.get<AdminUser>(authEndpoints.me, {
      headers: {
        Authorization: `Bearer ${accessToken}`,
      },
    })
  },
}
