import { useMemo, useState } from 'react'
import { CreateGraphModal } from '../features/graphs/components/CreateGraphModal'
import { DeleteGraphModal } from '../features/graphs/components/DeleteGraphModal'
import { EdgeEditorModal } from '../features/graphs/components/EdgeEditorModal'
import { GraphEditor } from '../features/graphs/components/GraphEditor'
import { GraphList } from '../features/graphs/components/GraphList'
import { NodeEditorModal } from '../features/graphs/components/NodeEditorModal'
import { useGraphs } from '../features/graphs/hooks/useGraphs'
import { graphApi } from '../features/graphs/services/graphApi'
import type { Graph, GraphEdge, GraphNode } from '../features/graphs/types/graph.types'
import { PageHeader } from '../shared/components/layout/PageHeader'
import { Section } from '../shared/components/layout/Section'
import { Button } from '../shared/components/ui/Button'
import { useDisclosure } from '../shared/hooks/useDisclosure'

export function GraphsPage() {
  const createGraphModal = useDisclosure()
  const deleteGraphModal = useDisclosure()
  const nodeModal = useDisclosure()
  const edgeModal = useDisclosure()
  const { data: graphs, error, isLoading, refetch } = useGraphs()
  const [edgeToEdit, setEdgeToEdit] = useState<GraphEdge | null>(null)
  const [graphToDelete, setGraphToDelete] = useState<Graph | null>(null)
  const [isDeletingGraph, setIsDeletingGraph] = useState(false)
  const [nodeToEdit, setNodeToEdit] = useState<GraphNode | null>(null)
  const [selectedGraphId, setSelectedGraphId] = useState<string | null>(null)

  const selectedGraph = useMemo(
    () => graphs?.find((graph) => graph.id === selectedGraphId) ?? graphs?.[0] ?? null,
    [graphs, selectedGraphId],
  )

  async function refreshGraphs() {
    await refetch()
  }

  function openDeleteGraph(graph: Graph) {
    setGraphToDelete(graph)
    deleteGraphModal.open()
  }

  async function confirmDeleteGraph() {
    if (!graphToDelete) {
      return
    }

    setIsDeletingGraph(true)
    await graphApi.deleteGraph(graphToDelete.id)
    setIsDeletingGraph(false)
    setSelectedGraphId(null)
    setGraphToDelete(null)
    deleteGraphModal.close()
    await refreshGraphs()
  }

  function openNodeEditor(node: GraphNode | null) {
    setNodeToEdit(node)
    nodeModal.open()
  }

  function openEdgeEditor(edge: GraphEdge | null) {
    setEdgeToEdit(edge)
    edgeModal.open()
  }

  async function saveNode(input: { label: string; type?: string; data: Record<string, unknown> }) {
    if (!selectedGraph) {
      return
    }

    if (nodeToEdit) {
      await graphApi.updateNode(selectedGraph.id, nodeToEdit.id, input)
    } else {
      await graphApi.createNode(selectedGraph.id, input)
    }

    nodeModal.close()
    await refreshGraphs()
  }

  async function saveEdge(input: {
    data: Record<string, unknown>
    label?: string
    source: string
    target: string
  }) {
    if (!selectedGraph) {
      return
    }

    if (edgeToEdit) {
      await graphApi.updateEdge(selectedGraph.id, edgeToEdit.id, input)
    } else {
      await graphApi.createEdge(selectedGraph.id, input)
    }

    edgeModal.close()
    await refreshGraphs()
  }

  async function deleteNode(node: GraphNode) {
    if (!selectedGraph) {
      return
    }

    await graphApi.deleteNode(selectedGraph.id, node.id)
    await refreshGraphs()
  }

  async function deleteEdge(edge: GraphEdge) {
    if (!selectedGraph) {
      return
    }

    await graphApi.deleteEdge(selectedGraph.id, edge.id)
    await refreshGraphs()
  }

  return (
    <div className="space-y-8">
      <PageHeader
        actions={<Button onClick={createGraphModal.open}>New graph</Button>}
        description="Manage graph definitions through the NexoraDB graph engine and edit dashboard graph records."
        title="Graphs"
      />
      <Section title="Available graphs">
        <GraphList
          error={error}
          graphs={graphs}
          isLoading={isLoading}
          onDelete={openDeleteGraph}
          onSelect={(graph) => setSelectedGraphId(graph.id)}
          selectedGraphId={selectedGraph?.id}
        />
      </Section>
      <Section title="Graph editor">
        <GraphEditor
          graph={selectedGraph}
          onAddEdge={() => openEdgeEditor(null)}
          onAddNode={() => openNodeEditor(null)}
          onDeleteEdge={deleteEdge}
          onDeleteNode={deleteNode}
          onEditEdge={openEdgeEditor}
          onEditNode={openNodeEditor}
        />
      </Section>
      <CreateGraphModal
        isOpen={createGraphModal.isOpen}
        onClose={createGraphModal.close}
        onCreated={refreshGraphs}
      />
      <DeleteGraphModal
        graph={graphToDelete}
        isDeleting={isDeletingGraph}
        isOpen={deleteGraphModal.isOpen}
        onClose={deleteGraphModal.close}
        onConfirm={confirmDeleteGraph}
      />
      <NodeEditorModal
        isOpen={nodeModal.isOpen}
        node={nodeToEdit}
        onClose={nodeModal.close}
        onSave={saveNode}
      />
      <EdgeEditorModal
        edge={edgeToEdit}
        isOpen={edgeModal.isOpen}
        nodes={selectedGraph?.nodes ?? []}
        onClose={edgeModal.close}
        onSave={saveEdge}
      />
    </div>
  )
}
