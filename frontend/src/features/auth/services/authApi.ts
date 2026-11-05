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
  logout(accessToken: string): Promise<void> {
    return httpClient.post<void>('/auth/logout', {}, { headers: { Authorization: `Bearer ${accessToken}` } })
  },
  changePassword(currentPassword: string, newPassword: string): Promise<{ message: string }> {
    return httpClient.post('/auth/change-password', { currentPassword, newPassword })
  },
  resetPassword(resetToken: string, newPassword: string): Promise<{ message: string }> {
    return httpClient.post('/auth/reset-password', { resetToken, newPassword })
  },
  getSetupState(): Promise<SetupStateResponse> {
    return httpClient.get<SetupStateResponse>(authEndpoints.setupState)
  },

  register(input: RegisterInput): Promise<AdminUser> {
    return httpClient.post<AdminUser, RegisterInput>(authEndpoints.register, input)
  },

  login(input: LoginInput): Promise<AuthResponse> {
    return httpClient.post<AuthResponse, LoginInput>(authEndpoints.login, input)
  },

  me(accessToken: string, signal?: AbortSignal): Promise<AdminUser> {
    return httpClient.get<AdminUser>(authEndpoints.me, {
      signal,
      headers: {
        Authorization: `Bearer ${accessToken}`,
      },
    })
  },
}
