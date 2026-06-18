export const documentEndpoints = {
  list: (collectionName: string) =>
    `/collections/${encodeURIComponent(collectionName)}/documents`,
  detail: (collectionName: string, documentId: string) =>
    `/collections/${encodeURIComponent(collectionName)}/documents/${encodeURIComponent(
      documentId,
    )}`,
  create: (collectionName: string) =>
    `/collections/${encodeURIComponent(collectionName)}/documents`,
  update: (collectionName: string, documentId: string) =>
    `/collections/${encodeURIComponent(collectionName)}/documents/${encodeURIComponent(
      documentId,
    )}`,
  delete: (collectionName: string, documentId: string) =>
    `/collections/${encodeURIComponent(collectionName)}/documents/${encodeURIComponent(
      documentId,
    )}`,
}
