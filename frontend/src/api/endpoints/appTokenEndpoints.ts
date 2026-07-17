export const appTokenEndpoints = {
  create: '/apps/tokens',
  list: '/apps/tokens',
  remove: (tokenId: string) => `/apps/tokens/${encodeURIComponent(tokenId)}`,
  scopes: '/apps/scopes',
}
