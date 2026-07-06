export interface AppScopeDefinition {
  scope: string
  label: string
  description: string
}

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

export const APP_SCOPE_CATALOG: AppScopeDefinition[] = [
  {
    scope: 'query:execute',
    label: 'Execute queries',
    description: 'Run NexoraQL through the external API.',
  },
  {
    scope: 'collections:read',
    label: 'Read collections',
    description: 'List and inspect collections.',
  },
  {
    scope: 'collections:write',
    label: 'Write collections',
    description: 'Create, rename, and delete collections.',
  },
  {
    scope: 'documents:read',
    label: 'Read documents',
    description: 'Fetch documents from collections.',
  },
  {
    scope: 'documents:write',
    label: 'Write documents',
    description: 'Insert, update, and delete documents.',
  },
  {
    scope: 'graphs:read',
    label: 'Read graphs',
    description: 'List graphs and graph metadata.',
  },
  {
    scope: 'graphs:write',
    label: 'Write graphs',
    description: 'Create and modify graph definitions.',
  },
  {
    scope: 'monitoring:read',
    label: 'Read monitoring',
    description: 'Access database health and traffic metrics.',
  },
  {
    scope: 'admin:apps',
    label: 'Manage app tokens',
    description: 'Issue tokens for other external applications.',
  },
]

export const APP_SCOPE_PRESETS: Record<string, string[]> = {
  'Query only': ['query:execute'],
  'Read only': [
    'query:execute',
    'collections:read',
    'documents:read',
    'graphs:read',
    'monitoring:read',
  ],
  'Read / write': [
    'query:execute',
    'collections:read',
    'collections:write',
    'documents:read',
    'documents:write',
    'graphs:read',
    'graphs:write',
  ],
  'Full access': APP_SCOPE_CATALOG.map((item) => item.scope),
}
