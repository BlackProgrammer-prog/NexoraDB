import type { Collection } from '../features/collections/types/collection.types'

export const mockCollections: Collection[] = [
  {
    createdAt: '2026-06-01T09:00:00.000Z',
    documentCount: 128,
    name: 'users',
    sizeBytes: 28672,
    updatedAt: '2026-06-15T14:12:00.000Z',
  },
  {
    createdAt: '2026-06-03T11:30:00.000Z',
    documentCount: 42,
    name: 'orders',
    sizeBytes: 18432,
    updatedAt: '2026-06-14T08:45:00.000Z',
  },
  {
    createdAt: '2026-06-08T16:20:00.000Z',
    documentCount: 17,
    name: 'products',
    sizeBytes: 12288,
    updatedAt: '2026-06-13T18:05:00.000Z',
  },
]
