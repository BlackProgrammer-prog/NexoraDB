import type { Graph } from '../features/graphs/types/graph.types'

export const mockGraphs: Graph[] = [
  {
    createdAt: '2026-06-05T10:00:00.000Z',
    description: 'Demo social graph for people and follow relationships.',
    edges: [
      {
        data: { since: '2026-01-10' },
        id: 'edge_001',
        label: 'follows',
        source: 'node_001',
        target: 'node_002',
      },
      {
        data: { since: '2026-02-14' },
        id: 'edge_002',
        label: 'follows',
        source: 'node_002',
        target: 'node_003',
      },
    ],
    id: 'graph_social',
    name: 'social',
    nodes: [
      {
        data: { handle: '@admin', role: 'admin' },
        id: 'node_001',
        label: 'Admin',
        type: 'user',
      },
      {
        data: { handle: '@mira', role: 'editor' },
        id: 'node_002',
        label: 'Mira',
        type: 'user',
      },
      {
        data: { handle: '@nexora', role: 'service' },
        id: 'node_003',
        label: 'Nexora',
        type: 'account',
      },
    ],
    updatedAt: '2026-06-18T13:25:00.000Z',
  },
]
