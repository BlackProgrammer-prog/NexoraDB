import { useCallback, useEffect, useMemo, useState } from 'react'
import type {
  ActiveConnection,
  ConnectionStatus,
  MonitoringMetrics,
  RequestsSample,
} from '../types/monitoring.types'

const maxSamples = 30
const defaultMetrics: MonitoringMetrics = {
  ramUsedBytes: 0,
  ssdUsedBytes: 0,
  requestsPerSecond: 0,
  activeConnections: [],
  receivedAt: Date.now(),
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' ? (value as Record<string, unknown>) : {}
}

function asNumber(value: unknown, fallback?: number): number | undefined {
  const numericValue = typeof value === 'string' ? Number(value) : value
  return typeof numericValue === 'number' && Number.isFinite(numericValue) ? numericValue : fallback
}

function pickNumber(source: Record<string, unknown>, keys: string[], fallback?: number): number | undefined {
  for (const key of keys) {
    if (source[key] !== undefined) {
      return asNumber(source[key], fallback)
    }
  }

  return fallback
}

function normalizeActiveConnections(value: unknown): ActiveConnection[] {
  if (Array.isArray(value)) {
    return value.map((connection, index) => {
      if (typeof connection === 'string' || typeof connection === 'number') {
        return { id: String(connection) }
      }

      const record = asRecord(connection)
      const id = record.id ?? record.connectionId ?? record.socketId ?? index + 1

      return {
        id: String(id),
        address: record.address ? String(record.address) : undefined,
        user: record.user ? String(record.user) : undefined,
      }
    })
  }

  const count = asNumber(value, 0) ?? 0
  return Array.from({ length: count }, (_, index) => ({ id: `connection-${index + 1}` }))
}

function normalizeMetrics(payload: unknown): MonitoringMetrics {
  const data = asRecord(payload)
  const ram = asRecord(data.ram)
  const ssd = asRecord(data.ssd)

  return {
    ramUsedBytes:
      pickNumber(data, ['ramUsedBytes', 'ramUsed', 'memoryUsedBytes'], pickNumber(ram, ['usedBytes', 'used'])) ??
      0,
    ramTotalBytes: pickNumber(
      data,
      ['ramTotalBytes', 'ramTotal', 'memoryTotalBytes'],
      pickNumber(ram, ['totalBytes', 'total']),
    ),
    ssdUsedBytes:
      pickNumber(data, ['ssdUsedBytes', 'ssdUsed', 'storageUsedBytes'], pickNumber(ssd, ['usedBytes', 'used'])) ??
      0,
    ssdTotalBytes: pickNumber(
      data,
      ['ssdTotalBytes', 'ssdTotal', 'storageTotalBytes'],
      pickNumber(ssd, ['totalBytes', 'total']),
    ),
    requestsPerSecond: pickNumber(data, ['requestsPerSecond', 'rps', 'requestRate']) ?? 0,
    activeConnections: normalizeActiveConnections(
      data.activeConnections ?? data.connections ?? data.connectionCount,
    ),
    receivedAt: Date.now(),
  }
}

function createSample(metrics: MonitoringMetrics): RequestsSample {
  return {
    label: new Intl.DateTimeFormat('en-US', {
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
    }).format(metrics.receivedAt),
    value: metrics.requestsPerSecond,
  }
}

export function useMonitoringSocket(url: string) {
  const [status, setStatus] = useState<ConnectionStatus>('connected')
  const [error, setError] = useState<string>()
  const [metrics, setMetrics] = useState<MonitoringMetrics>(defaultMetrics)
  const [samples, setSamples] = useState<RequestsSample[]>([])
  const [retryKey, setRetryKey] = useState(0)

  const reconnect = useCallback(() => {
    setStatus('connected')
    setError(undefined)
    setRetryKey((current) => current + 1)
  }, [])

  useEffect(() => {
    setStatus('connected')
    setError(undefined)

    const updateFakeMetrics = () => {
      const phase = Date.now() / 1000
      const nextMetrics = normalizeMetrics({
        ramUsedBytes: 3_200_000_000 + Math.round(Math.sin(phase) * 180_000_000),
        ramTotalBytes: 8_000_000_000,
        ssdUsedBytes: 128_000_000_000 + Math.round(Math.cos(phase / 2) * 3_000_000_000),
        ssdTotalBytes: 512_000_000_000,
        requestsPerSecond: Math.max(0, Math.round(24 + Math.sin(phase * 1.7) * 9)),
        activeConnections: [
          { id: 'mock-admin', user: 'admin', address: '127.0.0.1' },
          { id: 'mock-dashboard', user: 'dashboard', address: 'local' },
        ],
      })

      setMetrics(nextMetrics)
      setSamples((current) => [...current, createSample(nextMetrics)].slice(-maxSamples))
    }

    updateFakeMetrics()
    const intervalId = window.setInterval(updateFakeMetrics, 1000)

    return () => {
      window.clearInterval(intervalId)
    }

    /*
    const socket = new WebSocket(url)

    socket.addEventListener('open', () => {
      setStatus('connected')
      setError(undefined)
    })

    socket.addEventListener('message', (event) => {
      try {
        const nextMetrics = normalizeMetrics(JSON.parse(event.data))
        setMetrics(nextMetrics)
        setSamples((current) => [...current, createSample(nextMetrics)].slice(-maxSamples))
      } catch {
        setError('Received an invalid monitoring payload.')
      }
    })

    socket.addEventListener('error', () => {
      setStatus('disconnected')
      setError('Could not connect to the monitoring WebSocket.')
    })

    socket.addEventListener('close', () => {
      setStatus('disconnected')
    })

    return () => {
      socket.close()
    }
    */
  }, [retryKey, url])

  return useMemo(
    () => ({ error, metrics, reconnect, samples, status }),
    [error, metrics, reconnect, samples, status],
  )
}
