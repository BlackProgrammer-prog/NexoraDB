import { useEffect, useState } from 'react'
import { Button } from '../../../shared/components/ui/Button'
import { ErrorState } from '../../../shared/components/ui/ErrorState'
import { Input } from '../../../shared/components/ui/Input'
import { LoadingState } from '../../../shared/components/ui/LoadingState'
import { appTokenApi } from '../services/appTokenApi'
import { AppScopeSelector } from './AppScopeSelector'
import {
  APP_SCOPE_CATALOG,
  APP_SCOPE_PRESETS,
  type AppTokenResponse,
} from '../types/appToken.types'

interface CreateAppTokenFormProps {
  onCreated?: () => void
}

export function CreateAppTokenForm({ onCreated }: CreateAppTokenFormProps) {
  const [appId, setAppId] = useState('')
  const [appName, setAppName] = useState('')
  const [expiresInSeconds, setExpiresInSeconds] = useState('')
  const [availableScopes, setAvailableScopes] = useState<string[]>([])
  const [selectedScopes, setSelectedScopes] = useState<string[]>(['query:execute'])
  const [createdToken, setCreatedToken] = useState<AppTokenResponse | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [isLoadingScopes, setIsLoadingScopes] = useState(true)
  const [isCreating, setIsCreating] = useState(false)

  useEffect(() => {
    let isActive = true

    async function loadScopes() {
      setIsLoadingScopes(true)
      setError(null)

      try {
        const response = await appTokenApi.listScopes()
        if (isActive) {
          setAvailableScopes(response.scopes)
        }
      } catch (loadError) {
        if (isActive) {
          setError(loadError instanceof Error ? loadError.message : 'Could not load app scopes')
        }
      } finally {
        if (isActive) {
          setIsLoadingScopes(false)
        }
      }
    }

    void loadScopes()

    return () => {
      isActive = false
    }
  }, [])

  function applyPreset(presetName: string) {
    const preset = APP_SCOPE_PRESETS[presetName] ?? []
    setSelectedScopes(preset.filter((scope) => availableScopes.includes(scope)))
  }

  async function handleCreate() {
    if (!appId.trim() || !selectedScopes.length) {
      return
    }

    setIsCreating(true)
    setError(null)

    try {
      const token = await appTokenApi.createToken({
        appId: appId.trim(),
        appName: appName.trim() || undefined,
        scopes: selectedScopes,
        expiresInSeconds: expiresInSeconds ? Number(expiresInSeconds) : undefined,
      })
      setCreatedToken(token)
      onCreated?.()
    } catch (createError) {
      setError(createError instanceof Error ? createError.message : 'Could not create app token')
    } finally {
      setIsCreating(false)
    }
  }

  return (
    <div className="space-y-6">
      <div className="grid gap-4 md:grid-cols-2">
        <Input
          onChange={(event) => setAppId(event.target.value)}
          placeholder="App id, e.g. billing-service"
          value={appId}
        />
        <Input
          onChange={(event) => setAppName(event.target.value)}
          placeholder="Display name"
          value={appName}
        />
        <Input
          onChange={(event) => setExpiresInSeconds(event.target.value)}
          placeholder="Expires in seconds (optional)"
          value={expiresInSeconds}
        />
      </div>

      <div className="space-y-3">
        <div className="flex flex-wrap gap-2">
          {Object.keys(APP_SCOPE_PRESETS).map((presetName) => (
            <Button
              className="h-8 px-3 text-xs"
              key={presetName}
              onClick={() => applyPreset(presetName)}
              variant="secondary"
            >
              {presetName}
            </Button>
          ))}
        </div>

        {isLoadingScopes ? (
          <LoadingState label="Loading app permissions" />
        ) : (
          <AppScopeSelector
            availableScopes={availableScopes}
            catalog={APP_SCOPE_CATALOG}
            onChange={setSelectedScopes}
            selectedScopes={selectedScopes}
          />
        )}
      </div>

      {error ? <ErrorState message={error} /> : null}

      <Button disabled={isCreating || !appId.trim() || !selectedScopes.length} onClick={handleCreate}>
        {isCreating ? 'Creating token...' : 'Create app token'}
      </Button>

      {createdToken ? (
        <div className="space-y-3 rounded-lg border border-green-200 bg-green-50 p-4">
          <p className="text-sm font-semibold text-green-800">
            Token created for {createdToken.appName}
          </p>
          <p className="text-xs text-green-700">
            Copy this token now. It will not be shown again.
          </p>
          <pre className="overflow-auto rounded-md bg-slate-950 p-4 text-xs text-green-100">
            {createdToken.token}
          </pre>
          <Button onClick={() => navigator.clipboard.writeText(createdToken.token)}>
            Copy token
          </Button>
        </div>
      ) : null}
    </div>
  )
}
