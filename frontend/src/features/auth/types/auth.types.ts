export interface AdminUser {
  _id: string
  username: string
  email: string | null
  role: 'admin' | 'application'
  firstName: string | null
  lastName: string | null
  status: 'active' | 'disabled' | 'deleted'
  createdAt: number
  updatedAt: number
  lastLoginAt: number | null
  displayName: string
}

export interface LoginInput {
  username: string
  password: string
}

export interface RegisterInput {
  firstName: string
  lastName: string
  email: string
  password: string
  confirmPassword: string
}

export interface AuthResponse {
  accessToken: string
  tokenType: 'bearer'
  expiresIn: number
  user: AdminUser
}

export interface SetupStateResponse {
  needsSetup: boolean
}
