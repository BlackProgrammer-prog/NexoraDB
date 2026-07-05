import {
  CategoryScale,
  Chart,
  Filler,
  LineController,
  LineElement,
  LinearScale,
  PointElement,
  Tooltip,
  type ChartConfiguration,
} from 'chart.js'
import { useEffect, useMemo, useRef } from 'react'
import { API_BASE_URL } from '../../../api/client/apiConfig'
import { useAuth } from '../../auth/context/AuthContext'
import { Card } from '../../../shared/components/ui/Card'
import { cn } from '../../../shared/utils/cn'
import { useMonitoringSocket } from '../hooks/useMonitoringSocket'
import type { ConnectionStatus, RequestsSample } from '../types/monitoring.types'

Chart.register(CategoryScale, LinearScale, LineController, LineElement, PointElement, Filler, Tooltip)

const monitoringSocketUrl =
  import.meta.env.VITE_MONITORING_SOCKET_URL ?? API_BASE_URL

function formatBytes(bytes?: number): string {
  if (bytes === undefined || !Number.isFinite(bytes)) {
    return 'N/A'
  }

  if (bytes === 0) {
    return '0 B'
  }

  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  const exponent = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1)
  const value = bytes / 1024 ** exponent

  return `${value.toFixed(value >= 10 || exponent === 0 ? 0 : 1)} ${units[exponent]}`
}

function formatPercent(used: number, total?: number): string {
  if (!total) {
    return ''
  }

  return `${Math.round((used / total) * 100)}%`
}

function connectionLabel(status: ConnectionStatus): string {
  if (status === 'connected') {
    return 'Connect'
  }

  if (status === 'connecting') {
    return 'Connecting'
  }

  return 'Disconnected'
}

function RequestsLineChart({ samples }: { samples: RequestsSample[] }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const chartRef = useRef<Chart<'line'> | null>(null)

  useEffect(() => {
    if (!canvasRef.current) {
      return
    }

    const config: ChartConfiguration<'line'> = {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          {
            data: [],
            borderColor: '#16a34a',
            backgroundColor: 'rgba(22, 163, 74, 0.12)',
            borderWidth: 2,
            fill: true,
            pointRadius: 2,
            tension: 0.35,
          },
        ],
      },
      options: {
        animation: false,
        maintainAspectRatio: false,
        plugins: {
          tooltip: { intersect: false, mode: 'index' },
        },
        responsive: true,
        scales: {
          x: {
            grid: { display: false },
            ticks: { maxTicksLimit: 6 },
          },
          y: {
            beginAtZero: true,
            grid: { color: '#e2e8f0' },
            ticks: { precision: 0 },
          },
        },
      },
    }

    chartRef.current = new Chart(canvasRef.current, config)

    return () => {
      chartRef.current?.destroy()
      chartRef.current = null
    }
  }, [])

  useEffect(() => {
    if (!chartRef.current) {
      return
    }

    chartRef.current.data.labels = samples.map((sample) => sample.label)
    chartRef.current.data.datasets[0].data = samples.map((sample) => sample.value)
    chartRef.current.update()
  }, [samples])

  return <canvas aria-label="Requests per second chart" ref={canvasRef} />
}

export function MonitoringPanel() {
  const { accessToken } = useAuth()
  const { error, metrics, reconnect, samples, status } = useMonitoringSocket(
    monitoringSocketUrl,
    accessToken,
  )
  const connectionCount = metrics.activeConnections.length
  const isHealthy = status === 'connected' && metrics.databaseHealthy
  const connectionStatusClass = isHealthy ? 'text-green-700' : 'text-red-700'
  const statusMark = isHealthy ? '✓' : 'x'

  const cards = useMemo(
    () => [
      {
        label: 'RAM used',
        value: formatBytes(metrics.ramUsedBytes),
        detail: metrics.ramTotalBytes
          ? `${formatPercent(metrics.ramUsedBytes, metrics.ramTotalBytes)} of ${formatBytes(metrics.ramTotalBytes)}; used from nexoradb.so`
          : metrics.databaseHealthy ? 'From nexoradb.so' : 'Waiting for database health',
      },
      {
        label: 'SSD used',
        value: formatBytes(metrics.ssdUsedBytes),
        detail: metrics.ssdTotalBytes
          ? `${formatPercent(metrics.ssdUsedBytes, metrics.ssdTotalBytes)} of ${formatBytes(metrics.ssdTotalBytes)}; used from nexoradb.so`
          : metrics.databaseHealthy ? 'From nexoradb.so' : 'Waiting for database health',
      },
      {
        label: 'Requests / sec',
        value: String(metrics.requestsPerSecond),
        detail: `${samples.length} chart samples`,
      },
      {
        label: 'Active connections',
        value: String(connectionCount),
        detail: 'Apps active in the last 10 seconds',
      },
    ],
    [connectionCount, metrics, samples.length],
  )

  return (
    <section className="space-y-4">
      <div className="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <h2 className="text-lg font-semibold text-slate-950">Live health</h2>
          <p className="text-sm text-slate-500">
            Realtime database health from the FastAPI Socket.IO channel
          </p>
        </div>
        <button
          className={cn(
            'inline-flex h-10 items-center justify-center gap-2 rounded-md border px-4 text-sm font-semibold transition hover:bg-slate-50',
            isHealthy
              ? 'border-green-200 bg-green-50 text-green-800'
              : 'border-red-200 bg-red-50 text-red-800',
          )}
          onClick={reconnect}
          type="button"
        >
          <span
            className={cn(
              'flex size-5 items-center justify-center rounded-full bg-white text-sm font-bold',
              connectionStatusClass,
            )}
          >
            {statusMark}
          </span>
          {isHealthy ? connectionLabel(status) : 'Reconnect'}
        </button>
      </div>

      {error ? (
        <p className="rounded-md border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
          {error}
        </p>
      ) : null}

      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
        {cards.map((card) => (
          <Card key={card.label}>
            <p className="text-sm text-slate-500">{card.label}</p>
            <p className="mt-2 text-2xl font-semibold text-slate-950">{card.value}</p>
            <p className="mt-1 text-xs text-slate-500">{card.detail}</p>
          </Card>
        ))}
      </div>

      <div className="grid gap-4 xl:grid-cols-[minmax(0,2fr)_minmax(280px,1fr)]">
        <Card className="min-h-80">
          <div className="mb-4 flex items-center justify-between gap-3">
            <div>
              <h3 className="text-base font-semibold text-slate-950">Requests per second</h3>
              {/* <p className="text-sm text-slate-500">Line chart powered by Chart.js</p> */}
            </div>
            <p className="text-sm font-semibold text-green-700">{metrics.requestsPerSecond} rps</p>
          </div>
          <div className="h-56">
            <RequestsLineChart samples={samples} />
          </div>
        </Card>

        <Card className="min-h-80">
          <div className="mb-4 flex items-center justify-between gap-3">
            <div>
              <h3 className="text-base font-semibold text-slate-950">Active connections</h3>
              <p className="text-sm text-slate-500">Apps active in the last 10 seconds</p>
            </div>
            <span className="rounded-full bg-slate-100 px-2.5 py-1 text-xs font-semibold text-slate-700">
              {connectionCount}
            </span>
          </div>

          {metrics.activeConnections.length ? (
            <ul className="max-h-56 space-y-2 overflow-y-auto pr-1">
              {metrics.activeConnections.map((connection) => (
                <li
                  className="rounded-md border border-slate-200 px-3 py-2 text-sm text-slate-700"
                  key={connection.id}
                >
                  <span className="block font-medium text-slate-950">{connection.id}</span>
                  {connection.address || connection.user || connection.kind ? (
                    <span className="text-xs text-slate-500">
                      {[connection.user, connection.kind, connection.address]
                        .filter(Boolean)
                        .join(' | ')}
                    </span>
                  ) : null}
                </li>
              ))}
            </ul>
          ) : (
            <div className="flex h-44 items-center justify-center rounded-md border border-dashed border-slate-200 text-sm text-slate-500">
              No active connections yet
            </div>
          )}
        </Card>
      </div>
    </section>
  )
}
