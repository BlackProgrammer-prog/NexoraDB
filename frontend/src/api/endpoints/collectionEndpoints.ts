export const collectionEndpoints = {
  list: '/collections',
  create: '/collections',
  delete: (collectionName: string) =>
    `/collections/${encodeURIComponent(collectionName)}`,
}
