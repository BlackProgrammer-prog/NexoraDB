export type ConnectionStatus = 'connecting' | 'connected' | 'disconnected'

export interface ActiveConnection {
  id: string
  address?: string
  user?: string
}

export interface MonitoringMetrics {
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
