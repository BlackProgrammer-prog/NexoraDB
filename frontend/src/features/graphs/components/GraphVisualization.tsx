import cytoscape, { type Core, type EventObject } from 'cytoscape'
import { useEffect, useRef, useState } from 'react'
import { graphApi } from '../services/graphApi'
import type {
  GraphNodeDocument,
  GraphVisualizationData,
} from '../types/graph.types'
import { Button } from '../../../shared/components/ui/Button'
import { Card } from '../../../shared/components/ui/Card'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { stringifyJson } from '../../../shared/utils/json'

interface GraphVisualizationProps {
  graphId: string
  refreshKey: number
}

export function GraphVisualization({ graphId, refreshKey }: GraphVisualizationProps) {
  const containerRef = useRef<HTMLDivElement | null>(null)
  const graphRef = useRef<Core | null>(null)
  const [data, setData] = useState<GraphVisualizationData | null>(null)
  const [document, setDocument] = useState<GraphNodeDocument | null>(null)
  const [error, setError] = useState<Error | null>(null)
  const [isLoading, setIsLoading] = useState(true)
  const [isDocumentLoading, setIsDocumentLoading] = useState(false)

  useEffect(() => {
    let active = true
    queueMicrotask(() => {
      if (!active) return
      setIsLoading(true)
      setError(null)
      setDocument(null)
    })

    void graphApi
      .getVisualization(graphId)
      .then((value) => {
        if (active) setData(value)
      })
      .catch((loadError: unknown) => {
        if (active) {
          setData(null)
          setError(loadError instanceof Error ? loadError : new Error('Could not load graph'))
        }
      })
      .finally(() => {
        if (active) setIsLoading(false)
      })

    return () => {
      active = false
    }
  }, [graphId, refreshKey])

  useEffect(() => {
    if (!containerRef.current || !data?.nodes.length) return undefined

    const graph = cytoscape({
      container: containerRef.current,
      elements: [
        ...data.nodes.map((node) => ({ data: { ...node, id: node.id } })),
        ...data.edges.map((edge) => ({
          data: {
            ...edge,
            id: `edge:${edge.id}`,
            source: edge.source,
            target: edge.target,
          },
        })),
      ],
      minZoom: 0.05,
      maxZoom: 6,
      wheelSensitivity: 0.18,
      textureOnViewport: data.nodes.length > 400,
      hideEdgesOnViewport: data.nodes.length > 600,
      style: [
        {
          selector: 'node',
          style: {
            'background-color': '#16a34a',
            label: 'data(label)',
            color: '#0f172a',
            'font-size': 10,
            'text-margin-y': -10,
            'text-background-color': '#ffffff',
            'text-background-opacity': 0.85,
            'text-background-padding': '2px',
            width: 22,
            height: 22,
            'border-width': 2,
            'border-color': '#dcfce7',
          },
        },
        {
          selector: 'node:selected',
          style: {
            'background-color': '#0f766e',
            'border-color': '#5eead4',
            'border-width': 4,
          },
        },
        {
          selector: 'edge',
          style: {
            width: 1.5,
            'line-color': '#94a3b8',
            'target-arrow-color': '#64748b',
            'target-arrow-shape': 'triangle',
            'curve-style': 'bezier',
            opacity: 0.72,
          },
        },
      ],
      layout: {
        name: 'cose',
        animate: false,
        fit: true,
        padding: 45,
        nodeRepulsion: 180_000,
        idealEdgeLength: 90,
        edgeElasticity: 80,
        nestingFactor: 1.2,
        gravity: 0.22,
        numIter: data.nodes.length > 500 ? 500 : 1_000,
        initialTemp: 200,
        coolingFactor: 0.96,
        minTemp: 1,
        nodeOverlap: 28,
      },
    })

    graph.on('tap', 'node', (event: EventObject) => {
      const nodeId = String(event.target.id())
      setIsDocumentLoading(true)
      setDocument(null)
      void graphApi
        .getNodeDocument(graphId, nodeId)
        .then(setDocument)
        .catch((loadError: unknown) => {
          setError(
            loadError instanceof Error
              ? loadError
              : new Error(`Could not load document for ${nodeId}`),
          )
        })
        .finally(() => setIsDocumentLoading(false))
    })

    graphRef.current = graph
    const resizeObserver = new ResizeObserver(() => graph.resize())
    resizeObserver.observe(containerRef.current)

    return () => {
      resizeObserver.disconnect()
      graph.destroy()
      graphRef.current = null
    }
  }, [data, graphId])

  if (isLoading) return <LoadingState label="Loading graph visualization" />
  if (error && !data) return <ErrorState message={error.message} />
  if (!data?.nodes.length) return null

  function changeZoom(multiplier: number) {
    const graph = graphRef.current
    if (!graph) return
    graph.zoom(Math.max(0.05, Math.min(6, graph.zoom() * multiplier)))
    graph.center()
  }

  return (
    <Card>
      <div className="space-y-4">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div>
            <h3 className="text-base font-semibold text-slate-950">Interactive graph</h3>
            <p className="mt-1 text-sm text-slate-500">
              {data.nodeCount} nodes · {data.edgeCount} edges. Scroll to zoom, drag the canvas to
              pan, and click a node to inspect its source document.
            </p>
          </div>
          <div className="flex gap-2">
            <Button onClick={() => changeZoom(1.25)} variant="secondary">Zoom in</Button>
            <Button onClick={() => changeZoom(0.8)} variant="secondary">Zoom out</Button>
            <Button onClick={() => graphRef.current?.fit(undefined, 45)} variant="secondary">
              Fit
            </Button>
          </div>
        </div>

        <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_22rem]">
          <div
            className="h-[38rem] min-h-[28rem] overflow-hidden rounded-lg border border-slate-200 bg-slate-50"
            ref={containerRef}
          />
          <div className="min-h-48 rounded-lg border border-slate-200 bg-white p-4">
            <h4 className="text-sm font-semibold text-slate-950">Node document</h4>
            {isDocumentLoading ? <LoadingState label="Loading node document" /> : null}
            {!isDocumentLoading && document ? (
              <div className="mt-3 space-y-3">
                <div className="flex flex-wrap gap-2 text-xs text-slate-500">
                  <span className="rounded bg-slate-100 px-2 py-1">{document.nodeId}</span>
                  {document.nodeType ? (
                    <span className="rounded bg-green-50 px-2 py-1 text-green-700">
                      {document.nodeType}
                    </span>
                  ) : null}
                  {document.collection ? (
                    <span className="rounded bg-slate-100 px-2 py-1">{document.collection}</span>
                  ) : null}
                </div>
                <pre className="max-h-[30rem] overflow-auto rounded-md bg-slate-950 p-3 text-xs text-green-100">
                  {stringifyJson(document.document)}
                </pre>
              </div>
            ) : null}
            {!isDocumentLoading && !document ? (
              <p className="mt-3 text-sm text-slate-500">Select a node to load its document.</p>
            ) : null}
            {error && data ? <div className="mt-3"><ErrorState message={error.message} /></div> : null}
          </div>
        </div>
      </div>
    </Card>
  )
}
