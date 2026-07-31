import { useCallback, useEffect, useState } from 'react'
import { Badge } from '../../../shared/components/ui/Badge'
import { Button } from '../../../shared/components/ui/Button'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { appTokenApi } from '../services/appTokenApi'
import type { StoredAppToken } from '../types/appToken.types'

interface AppTokenListProps {
  refreshKey: number
}

function formatDate(timestamp: number | null) {
  if (timestamp === null) return 'Never'
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
  }).format(new Date(timestamp * 1000))
}

function maskToken(token: string) {
  if (token.length < 24) return token
  return `${token.slice(0, 14)}••••••••${token.slice(-8)}`
}

export function AppTokenList({ refreshKey }: AppTokenListProps) {
  const [tokens, setTokens] = useState<StoredAppToken[]>([])
  const [error, setError] = useState<string | null>(null)
  const [isLoading, setIsLoading] = useState(true)
  const [copiedId, setCopiedId] = useState<string | null>(null)
  const [deletingId, setDeletingId] = useState<string | null>(null)

  const loadTokens = useCallback(async () => {
    setIsLoading(true)
    setError(null)
    try {
      setTokens(await appTokenApi.listTokens())
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : 'Could not load app tokens')
    } finally {
      setIsLoading(false)
    }
  }, [])

  useEffect(() => {
    void loadTokens()
  }, [loadTokens, refreshKey])

  async function copyToken(token: StoredAppToken) {
    try {
      await navigator.clipboard.writeText(token.token)
      setCopiedId(token.id)
      window.setTimeout(() => setCopiedId(null), 1800)
    } catch {
      setError('Browser denied clipboard access. Copy the token from the creation result.')
    }
  }

  async function deleteToken(token: StoredAppToken) {
    if (!window.confirm(`Revoke the token for ${token.appName}? This cannot be undone.`)) {
      return
    }
    setDeletingId(token.id)
    setError(null)
    try {
      await appTokenApi.deleteToken(token.id)
      setTokens((current) => current.filter((item) => item.id !== token.id))
    } catch (deleteError) {
      setError(deleteError instanceof Error ? deleteError.message : 'Could not revoke token')
    } finally {
      setDeletingId(null)
    }
  }

  if (isLoading) return <LoadingState label="Loading application tokens" />
  if (error && !tokens.length) return <ErrorState message={error} />
  if (!tokens.length) {
    return <p className="text-sm text-slate-500">No stored application tokens yet.</p>
  }

  return (
    <div className="space-y-4">
      {error ? <ErrorState message={error} /> : null}
      <div className="grid gap-3 md:hidden">
        {tokens.map((token) => (
          <article className="min-w-0 rounded-lg border border-slate-200 bg-white p-4" key={token.id}>
            <div className="flex items-start justify-between gap-3">
              <div className="min-w-0">
                <p className="truncate font-medium text-slate-900">{token.appName}</p>
                <p className="truncate text-xs text-slate-500">{token.appId}</p>
              </div>
              <Badge className={token.status === 'expired' ? 'bg-red-50 text-red-700 ring-red-100' : undefined}>
                {token.status}
              </Badge>
            </div>
            <p className="mt-3 overflow-hidden text-ellipsis whitespace-nowrap rounded bg-slate-50 px-2 py-2 font-mono text-xs text-slate-600">
              {maskToken(token.token)}
            </p>
            <div className="mt-3 flex flex-wrap gap-1.5">
              {token.scopes.map((scope) => <Badge key={scope}>{scope}</Badge>)}
            </div>
            <dl className="mt-3 grid grid-cols-[4.5rem_1fr] gap-y-1 text-xs">
              <dt className="text-slate-500">Created</dt><dd className="text-slate-700">{formatDate(token.createdAt)}</dd>
              <dt className="text-slate-500">Expires</dt><dd className="text-slate-700">{formatDate(token.expiresAt)}</dd>
            </dl>
            <div className="mt-4 grid grid-cols-2 gap-2">
              <Button onClick={() => copyToken(token)} variant="secondary">
                {copiedId === token.id ? 'Copied' : 'Copy token'}
              </Button>
              <Button className="text-red-700" disabled={deletingId === token.id} onClick={() => deleteToken(token)} variant="ghost">
                {deletingId === token.id ? 'Revoking…' : 'Revoke'}
              </Button>
            </div>
          </article>
        ))}
      </div>
      <div className="hidden overflow-x-auto md:block">
        <table className="min-w-full divide-y divide-slate-200 text-left text-sm">
          <thead className="bg-slate-50 text-xs uppercase tracking-wide text-slate-500">
            <tr>
              <th className="px-4 py-3">Application</th>
              <th className="px-4 py-3">Token</th>
              <th className="px-4 py-3">Permissions</th>
              <th className="px-4 py-3">Created</th>
              <th className="px-4 py-3">Expires</th>
              <th className="px-4 py-3">Status</th>
              <th className="px-4 py-3 text-right">Action</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-slate-100">
            {tokens.map((token) => (
              <tr key={token.id} className="align-top">
                <td className="px-4 py-4">
                  <div className="font-medium text-slate-900">{token.appName}</div>
                  <div className="text-xs text-slate-500">{token.appId}</div>
                </td>
                <td className="px-4 py-4 font-mono text-xs text-slate-600">
                  {maskToken(token.token)}
                </td>
                <td className="max-w-sm px-4 py-4">
                  <div className="flex flex-wrap gap-1.5">
                    {token.scopes.map((scope) => (
                      <Badge key={scope}>{scope}</Badge>
                    ))}
                  </div>
                </td>
                <td className="whitespace-nowrap px-4 py-4 text-slate-600">
                  {formatDate(token.createdAt)}
                </td>
                <td className="whitespace-nowrap px-4 py-4 text-slate-600">
                  {formatDate(token.expiresAt)}
                </td>
                <td className="px-4 py-4">
                  <Badge
                    className={
                      token.status === 'expired'
                        ? 'bg-red-50 text-red-700 ring-red-100'
                        : undefined
                    }
                  >
                    {token.status}
                  </Badge>
                </td>
                <td className="px-4 py-4 text-right">
                  <div className="flex justify-end gap-2">
                    <Button
                      className="h-8 px-3 text-xs"
                      onClick={() => copyToken(token)}
                      variant="secondary"
                    >
                      {copiedId === token.id ? 'Copied' : 'Copy'}
                    </Button>
                    <Button
                      className="h-8 px-3 text-xs text-red-700"
                      disabled={deletingId === token.id}
                      onClick={() => deleteToken(token)}
                      variant="ghost"
                    >
                      {deletingId === token.id ? 'Revoking…' : 'Revoke'}
                    </Button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
