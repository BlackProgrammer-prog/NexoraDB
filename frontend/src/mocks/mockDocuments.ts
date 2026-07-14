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
  {
    collectionName: 'posts',
    createdAt: '2026-06-16T08:00:00.000Z',
    data: { authorId: 'usr_001', status: 'published', title: 'Welcome to NexoraDB' },
    id: 'post_001',
    updatedAt: '2026-06-18T10:05:00.000Z',
  },
  {
    collectionName: 'posts',
    createdAt: '2026-06-16T09:00:00.000Z',
    data: { authorId: 'usr_001', status: 'draft', title: 'Graph traversal notes' },
    id: 'post_002',
    updatedAt: '2026-06-18T10:15:00.000Z',
  },
  {
    collectionName: 'comments',
    createdAt: '2026-06-17T10:30:00.000Z',
    data: { body: 'Looks good.', postId: 'post_001', userId: 'usr_001' },
    id: 'comment_001',
    updatedAt: '2026-06-18T11:30:00.000Z',
  },
  {
    collectionName: 'comments',
    createdAt: '2026-06-17T11:00:00.000Z',
    data: { body: 'Ship the mock first.', postId: 'post_002', userId: 'usr_001' },
    id: 'comment_002',
    updatedAt: '2026-06-18T11:45:00.000Z',
  },
  {
    collectionName: 'follows',
    createdAt: '2026-06-18T12:00:00.000Z',
    data: { source: 'usr_001', target: 'usr_002' },
    id: 'follow_001',
    updatedAt: '2026-06-18T12:45:00.000Z',
  },
  {
    collectionName: 'follows',
    createdAt: '2026-06-18T12:10:00.000Z',
    data: { source: 'usr_002', target: 'usr_003' },
    id: 'follow_002',
    updatedAt: '2026-06-18T12:50:00.000Z',
  },
]
