import type { DocumentRecord } from '../features/documents/types/document.types'

export const mockDocuments: DocumentRecord[] = [
  {
    collectionName: 'users',
    createdAt: '2026-06-10T10:00:00.000Z',
    data: { email: 'admin@nexoradb.local', role: 'admin', status: 'active' },
    id: 'usr_001',
    updatedAt: '2026-06-15T12:00:00.000Z',
  },
  {
    collectionName: 'orders',
    createdAt: '2026-06-11T12:30:00.000Z',
    data: { amount: 149.99, currency: 'USD', status: 'paid' },
    id: 'ord_001',
    updatedAt: '2026-06-14T09:20:00.000Z',
  },
  {
    collectionName: 'products',
    createdAt: '2026-06-12T08:15:00.000Z',
    data: { name: 'Vector index', price: 79, stock: 12 },
    id: 'prd_001',
    updatedAt: '2026-06-13T17:40:00.000Z',
  },
]
