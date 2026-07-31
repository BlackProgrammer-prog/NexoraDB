import { useCallback, useEffect, useMemo, useState } from 'react'
import { io } from 'socket.io-client'
import type {
  ActiveConnection,
  ConnectionStatus,
  MonitoringMetrics,
  RequestsSample,
} from '../types/monitoring.types'

const maxSamples = 30
const defaultMetrics: MonitoringMetrics = {
  databaseHealthy: false,
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
        activeWithinSeconds: asNumber(record.activeWithinSeconds),
        kind: record.kind ? String(record.kind) : undefined,
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
    databaseHealthy: Boolean(
      data.databaseEngineHealthy ?? data.databaseHealthy ?? data.dbHealthy ?? data.healthy,
    ),
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
    receivedAt: pickNumber(data, ['receivedAt'], Date.now()) ?? Date.now(),
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

export function useMonitoringSocket(url: string, accessToken: string | null) {
  const [status, setStatus] = useState<ConnectionStatus>('connecting')
  const [error, setError] = useState<string>()
  const [metrics, setMetrics] = useState<MonitoringMetrics>(defaultMetrics)
  const [samples, setSamples] = useState<RequestsSample[]>([])
  const [retryKey, setRetryKey] = useState(0)

  const reconnect = useCallback(() => {
    setStatus('connecting')
    setError(undefined)
    setRetryKey((current) => current + 1)
  }, [])

  useEffect(() => {
    if (!accessToken) {
      setStatus('disconnected')
      setError('Sign in again to monitor database health.')
      setMetrics(defaultMetrics)
      setSamples([])
      return undefined
    }

    setStatus('connecting')
    setError(undefined)

    const socket = io(url, {
      auth: { token: accessToken },
      path: '/socket.io',
      reconnection: false,
      transports: ['websocket'],
    })

    function handleMetrics(payload: unknown) {
      try {
        const nextMetrics = normalizeMetrics(payload)
        setError(undefined)
        setStatus('connected')
        setMetrics(nextMetrics)
        setSamples((current) => [...current, createSample(nextMetrics)].slice(-maxSamples))
      } catch {
        setError('Received an invalid monitoring payload.')
      }
    }

    socket.on('connect', () => {
      setStatus('connected')
      setError(undefined)
      socket.emit('metrics:refresh')
    })

    socket.on('metrics', handleMetrics)

    socket.on('connect_error', () => {
      setStatus('disconnected')
      setError('Could not connect to live monitoring.')
    })

    socket.on('disconnect', () => {
      setStatus('disconnected')
    })

    const heartbeatId = window.setInterval(() => {
      if (socket.connected) {
        socket.emit('heartbeat')
      }
    }, 5000)

    return () => {
      window.clearInterval(heartbeatId)
      socket.disconnect()
    }

  }, [accessToken, retryKey, url])

  return useMemo(
    () => ({ error, metrics, reconnect, samples, status }),
    [error, metrics, reconnect, samples, status],
  )
}
