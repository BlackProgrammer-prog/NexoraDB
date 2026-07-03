export const collectionEndpoints = {
  list: '/collections',
  create: '/collections',
  update: (collectionName: string) =>
    `/collections/${encodeURIComponent(collectionName)}`,
  delete: (collectionName: string) =>
    `/collections/${encodeURIComponent(collectionName)}`,
}
