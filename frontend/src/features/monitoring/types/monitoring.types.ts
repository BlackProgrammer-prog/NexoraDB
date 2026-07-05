export type ConnectionStatus = 'connecting' | 'connected' | 'disconnected'

export interface ActiveConnection {
  id: string
  address?: string
  user?: string
  kind?: string
  activeWithinSeconds?: number
}

export interface MonitoringMetrics {
  databaseHealthy: boolean
  ramUsedBytes: number
  ramTotalBytes?: number
  ssdUsedBytes: number
  ssdTotalBytes?: number
  requestsPerSecond: number
  activeConnections: ActiveConnection[]
  receivedAt: number
}

export interface RequestsSample {
  label: string
  value: number
}
