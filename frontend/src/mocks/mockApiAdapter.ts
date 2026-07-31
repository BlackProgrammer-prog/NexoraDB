import type {
  Collection,
  CreateCollectionInput,
  UpdateCollectionInput,
} from '../features/collections/types/collection.types'
import type {
  CreateDocumentInput,
  DocumentRecord,
  UpdateDocumentInput,
} from '../features/documents/types/document.types'
import type {
  CreateGraphEdgeInput,
  CreateGraphInput,
  CreateGraphNodeInput,
  Graph,
  UpdateGraphEdgeInput,
  UpdateGraphNodeInput,
} from '../features/graphs/types/graph.types'
import type { QueryRequest, QueryResult } from '../features/query/types/query.types'
import { mockCollections } from './mockCollections'
import { mockDocuments } from './mockDocuments'
import { mockGraphs } from './mockGraphs'

const latencyMs = 120

function wait() {
  return new Promise((resolve) => {
    window.setTimeout(resolve, latencyMs)
  })
}

let collections = [...mockCollections]
let documents = [...mockDocuments]
let graphs = [...mockGraphs]

function refreshCollectionStats() {
  collections = collections.map((collection) => {
    const collectionDocuments = documents.filter(
      (document) => document.collectionName === collection.name,
    )

    return {
      ...collection,
      documentCount: collectionDocuments.length,
      sizeBytes: collectionDocuments.reduce(
        (total, document) => total + JSON.stringify(document.data).length,
        0,
      ),
    }
  })
}

function getQueryRows(collectionName: string) {
  return documents
    .filter((document) => document.collectionName === collectionName)
    .map((document) => ({
      id: document.id,
      ...document.data,
      updatedAt: document.updatedAt,
    }))
}

function makeColumns(rows: Record<string, unknown>[]) {
  return Array.from(new Set(rows.flatMap((row) => Object.keys(row))))
}

export const mockApiAdapter = {
  async listCollections(): Promise<Collection[]> {
    await wait()
    return [...collections]
  },

  async createCollection(input: CreateCollectionInput): Promise<Collection> {
    await wait()
    const timestamp = new Date().toISOString()
    const collection: Collection = {
      createdAt: timestamp,
      documentCount: 0,
      name: input.name,
      sizeBytes: 0,
      updatedAt: timestamp,
    }

    collections = [...collections, collection]
    return collection
  },

  async updateCollection(
    collectionName: string,
    input: UpdateCollectionInput,
  ): Promise<Collection> {
    await wait()
    const timestamp = new Date().toISOString()
    const existingCollection = collections.find(
      (collection) => collection.name === collectionName,
    )

    if (!existingCollection) {
      throw new Error('Collection not found')
    }

    collections = collections.map((collection) =>
      collection.name === collectionName
        ? { ...collection, name: input.name, updatedAt: timestamp }
        : collection,
    )
    documents = documents.map((document) =>
      document.collectionName === collectionName
        ? { ...document, collectionName: input.name, updatedAt: timestamp }
        : document,
    )

    return collections.find((collection) => collection.name === input.name)!
  },

  async deleteCollection(collectionName: string): Promise<void> {
    await wait()
    collections = collections.filter((collection) => collection.name !== collectionName)
    documents = documents.filter((document) => document.collectionName !== collectionName)
  },

  async listDocuments(collectionName: string): Promise<DocumentRecord[]> {
    await wait()
    return documents.filter((document) => document.collectionName === collectionName)
  },

  async getDocument(collectionName: string, documentId: string): Promise<DocumentRecord | null> {
    await wait()
    return (
      documents.find(
        (document) =>
          document.collectionName === collectionName && document.id === documentId,
      ) ?? null
    )
  },

  async createDocument(
    collectionName: string,
    input: CreateDocumentInput,
  ): Promise<DocumentRecord> {
    await wait()
    const timestamp = new Date().toISOString()
    const document: DocumentRecord = {
      collectionName,
      createdAt: timestamp,
      data: input.data,
      id: crypto.randomUUID(),
      updatedAt: timestamp,
    }

    documents = [...documents, document]
    refreshCollectionStats()
    return document
  },

  async updateDocument(
    collectionName: string,
    documentId: string,
    input: UpdateDocumentInput,
  ): Promise<DocumentRecord> {
    await wait()
    const timestamp = new Date().toISOString()
    const nextDocuments = documents.map((document) =>
      document.collectionName === collectionName && document.id === documentId
        ? { ...document, data: input.data, updatedAt: timestamp }
        : document,
    )

    documents = nextDocuments
    refreshCollectionStats()
    const updatedDocument = documents.find(
      (document) => document.collectionName === collectionName && document.id === documentId,
    )

    if (!updatedDocument) {
      throw new Error('Document not found')
    }

    return updatedDocument
  },

  async deleteDocument(collectionName: string, documentId: string): Promise<void> {
    await wait()
    documents = documents.filter(
      (document) =>
        document.collectionName !== collectionName || document.id !== documentId,
    )
    refreshCollectionStats()
  },

  async executeQuery(input: QueryRequest): Promise<QueryResult> {
    await wait()
    const query = input.query.trim().replace(/;+$/, '').trim().replace(/\s+/g, ' ')
    const normalizedQuery = query.toUpperCase()

    if (!query) {
      throw new Error('Query cannot be empty')
    }

    if (normalizedQuery === 'SHOW COLLECTIONS') {
      const rows = collections.map((collection) => ({
        documents: collection.documentCount,
        name: collection.name,
        sizeBytes: collection.sizeBytes,
        updatedAt: collection.updatedAt,
      }))

      return { columns: makeColumns(rows), executionTimeMs: 0, raw: rows, rows }
    }

    if (normalizedQuery === 'SHOW GRAPHS') {
      const rows = graphs.map((graph) => ({
        edges: graph.edges.length,
        id: graph.id,
        name: graph.name,
        nodes: graph.nodes.length,
        updatedAt: graph.updatedAt,
      }))

      return { columns: makeColumns(rows), executionTimeMs: 0, raw: rows, rows }
    }

    const findCollectionMatch = normalizedQuery.match(/^FIND (USERS|POSTS|COMMENTS|FOLLOWS)$/)
    if (findCollectionMatch) {
      const collectionName = findCollectionMatch[1].toLowerCase()
      const rows = getQueryRows(collectionName)

      return {
        columns: makeColumns(rows),
        executionTimeMs: 0,
        raw: documents.filter((document) => document.collectionName === collectionName),
        rows,
      }
    }

    if (normalizedQuery === 'FIND GRAPH SOCIAL') {
      const graph = graphs.find((item) => item.name === 'social')
      if (!graph) {
        throw new Error('Graph "social" was not found')
      }

      const rows = [
        { count: graph.nodes.length, type: 'nodes' },
        { count: graph.edges.length, type: 'edges' },
      ]

      return { columns: makeColumns(rows), executionTimeMs: 0, raw: graph, rows }
    }

    throw new Error(`Unknown mock query "${query}". Try SHOW COLLECTIONS or SHOW GRAPHS.`)
  },

  async listGraphs(): Promise<Graph[]> {
    await wait()
    return graphs.map((graph) => ({ ...graph, edges: [...graph.edges], nodes: [...graph.nodes] }))
  },

  async createGraph(input: CreateGraphInput): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    const graph: Graph = {
      createdAt: timestamp,
      description: input.description,
      edges: [],
      id: crypto.randomUUID(),
      name: input.name,
      nodes: [],
      updatedAt: timestamp,
    }

    graphs = [...graphs, graph]
    return graph
  },

  async deleteGraph(graphId: string): Promise<void> {
    await wait()
    graphs = graphs.filter((graph) => graph.id !== graphId)
  },

  async createGraphNode(
    graphId: string,
    input: CreateGraphNodeInput,
  ): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    const node = { ...input, id: crypto.randomUUID() }

    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? { ...graph, nodes: [...graph.nodes, node], updatedAt: timestamp }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },

  async updateGraphNode(
    graphId: string,
    nodeId: string,
    input: UpdateGraphNodeInput,
  ): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? {
            ...graph,
            nodes: graph.nodes.map((node) =>
              node.id === nodeId ? { ...node, ...input } : node,
            ),
            updatedAt: timestamp,
          }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },

  async deleteGraphNode(graphId: string, nodeId: string): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? {
            ...graph,
            edges: graph.edges.filter(
              (edge) => edge.source !== nodeId && edge.target !== nodeId,
            ),
            nodes: graph.nodes.filter((node) => node.id !== nodeId),
            updatedAt: timestamp,
          }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },

  async createGraphEdge(
    graphId: string,
    input: CreateGraphEdgeInput,
  ): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    const edge = { ...input, id: crypto.randomUUID() }
    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? { ...graph, edges: [...graph.edges, edge], updatedAt: timestamp }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },

  async updateGraphEdge(
    graphId: string,
    edgeId: string,
    input: UpdateGraphEdgeInput,
  ): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? {
            ...graph,
            edges: graph.edges.map((edge) =>
              edge.id === edgeId ? { ...edge, ...input } : edge,
            ),
            updatedAt: timestamp,
          }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },

  async deleteGraphEdge(graphId: string, edgeId: string): Promise<Graph> {
    await wait()
    const timestamp = new Date().toISOString()
    graphs = graphs.map((graph) =>
      graph.id === graphId
        ? {
            ...graph,
            edges: graph.edges.filter((edge) => edge.id !== edgeId),
            updatedAt: timestamp,
          }
        : graph,
    )

    return graphs.find((graph) => graph.id === graphId)!
  },
}
