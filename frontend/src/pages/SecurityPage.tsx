import { useEffect, useState, type FormEvent } from 'react'
import { httpClient } from '../api/client/httpClient'
import { authApi } from '../features/auth/services/authApi'
import { useAuth } from '../features/auth/context/useAuth'
import type { AdminUser } from '../features/auth/types/auth.types'
import { Button } from '../shared/components/ui/Button'
import { Input } from '../shared/components/ui/Input'
import { CollectionGrantsEditor, type CollectionGrants } from '../shared/components/ui/CollectionGrantsEditor'

export function SecurityPage() {
  const { user, logout } = useAuth()
  const [users, setUsers] = useState<AdminUser[]>([])
  const [name, setName] = useState('')
  const [password, setPassword] = useState('')
  const [createRoles, setCreateRoles] = useState('')
  const [createSuperuser, setCreateSuperuser] = useState(false)
  const [createGrants, setCreateGrants] = useState<CollectionGrants>({})
  const [current, setCurrent] = useState('')
  const [replacement, setReplacement] = useState('')
  const [selected, setSelected] = useState('')
  const [roles, setRoles] = useState('')
  const [grants, setGrants] = useState('{}')
  const [roleName, setRoleName] = useState('')
  const [parents, setParents] = useState('')
  const [factor, setFactor] = useState('')
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)
  const [details, setDetails] = useState('')
  const [recoveryCodes, setRecoveryCodes] = useState<string[]>([])
  useEffect(() => {
    if (!factor) return
    const timer = window.setTimeout(() => setFactor(''), 60_000)
    return () => window.clearTimeout(timer)
  }, [factor])
  useEffect(() => {
    if (!recoveryCodes.length) return
    const timer = window.setTimeout(() => setRecoveryCodes([]), 60_000)
    return () => window.clearTimeout(timer)
  }, [recoveryCodes])
  async function run(action: () => Promise<void>) {
    setBusy(true); setMessage('')
    try { await action(); setMessage('Saved') }
    catch (error) { setMessage(error instanceof Error ? error.message : 'Security operation failed') }
    finally { setBusy(false); setPassword(''); setCurrent(''); setReplacement('') }
  }
  async function loadUsers() { setUsers(await httpClient.get<AdminUser[]>('/security/users')) }
  async function change(event: FormEvent) {
    event.preventDefault()
    await run(async () => { await authApi.changePassword(current, replacement); logout() })
  }
  const target = users.find((entry) => entry._id === selected)
  const roleList = (value: string) => value.split(',').map((part) => part.trim()).filter(Boolean)
  return <div className="max-w-4xl space-y-7">
    <h1 className="text-2xl font-semibold">Account and access</h1>
    <p role="status" className="text-sm text-slate-600">{message}</p>
    <form onSubmit={(event) => void change(event)} className="space-y-3 rounded-xl border bg-white p-5">
      <h2 className="font-semibold">Change your password</h2>
      <label className="block text-sm">Current password<Input type="password" autoComplete="current-password" value={current}
        onChange={(event) => setCurrent(event.target.value)} required maxLength={1024} /></label>
      <label className="block text-sm">New password<Input type="password" autoComplete="new-password" value={replacement}
        onChange={(event) => setReplacement(event.target.value)} required minLength={15} maxLength={1024} /></label>
      <p className="text-xs text-slate-500">At least 15 characters. A successful change signs out every session and revokes delegated credentials.</p>
      <Button disabled={busy} type="submit">Change password and sign out</Button>
    </form>
    {user?.protectedRoot && <section className="space-y-3 rounded-xl border bg-white p-5">
      <h2 className="font-semibold">Offline recovery factors</h2>
      <p className="text-sm text-slate-500">Enter your current password above, then replace all previous factors. Save the new factors offline; they are shown once and hidden after 60 seconds.</p>
      <Button disabled={busy || !current} onClick={() => void run(async () => {
        const result = await httpClient.post<{ recoveryCodes: string[] }>('/security/recovery/rotate', { currentPassword: current })
        setRecoveryCodes(result.recoveryCodes)
      })}>Replace owner recovery factors</Button>
      {recoveryCodes.length > 0 && <div className="space-y-1 rounded bg-amber-50 p-3">
        {recoveryCodes.map((code) => <output key={code} className="block break-all font-mono text-xs">{code}</output>)}
        <button type="button" className="text-sm underline" onClick={() => setRecoveryCodes([])}>Hide factors now</button>
      </div>}
    </section>}
    {user?.role === 'admin' && <>
      <section className="space-y-3 rounded-xl border bg-white p-5">
        <h2 className="font-semibold">Create a user</h2>
        <p className="text-sm text-slate-500">Create the account, initial password and permissions together. Empty roles and grants give no data access.</p>
        <Input aria-label="New username" placeholder="Username" value={name} onChange={(event) => setName(event.target.value)} maxLength={64} />
        <Input aria-label="Initial password" placeholder="Initial password" type="password" autoComplete="new-password" value={password}
          onChange={(event) => setPassword(event.target.value)} minLength={15} maxLength={1024} />
        <label className="block text-sm">Initial roles (comma-separated)
          <Input value={createRoles} onChange={(event) => setCreateRoles(event.target.value)} maxLength={1024} />
        </label>
        <CollectionGrantsEditor label="Initial collection permissions" value={createGrants} onChange={setCreateGrants} />
        {user.protectedRoot && <label className="block text-sm"><input type="checkbox" checked={createSuperuser}
          onChange={(event) => setCreateSuperuser(event.target.checked)} /> Grant SuperUser to the new account</label>}
        <p className="text-xs text-slate-500">Only the initial protected owner can grant SuperUser, regardless of the owner's username.</p>
        <Button disabled={busy || !name || password.length < 15} onClick={() => void run(async () => {
          const assigned = roleList(createRoles)
          if (createSuperuser && !assigned.includes('superuser')) assigned.push('superuser')
          if (!user.protectedRoot && assigned.includes('superuser')) throw new Error('Only the initial protected owner can grant SuperUser')
          await httpClient.post('/security/users', { username: name, password, roles: assigned, grants: createGrants })
          setName(''); setCreateRoles(''); setCreateSuperuser(false); setCreateGrants({}); await loadUsers()
        })}>Create user</Button>
      </section>
      <section className="space-y-3 rounded-xl border bg-white p-5">
        <h2 className="font-semibold">Users and permissions</h2>
        <Button disabled={busy} onClick={() => void run(loadUsers)}>Load users</Button>
        <select aria-label="User" className="block w-full rounded border p-2" value={selected} onChange={(event) => {
          const entry = users.find((candidate) => candidate._id === event.target.value)
          setSelected(event.target.value); setRoles(entry?.roles?.join(', ') ?? ''); setGrants(JSON.stringify(entry?.grants ?? {}, null, 2))
        }}>
          <option value="">Select a user</option>
          {users.map((entry) => <option key={entry._id} value={entry._id}>{entry.username} · {entry.status}{entry.protectedRoot ? ' · protected owner' : ''}</option>)}
        </select>
        <label className="block text-sm">Roles (comma-separated)<Input value={roles} onChange={(event) => setRoles(event.target.value)} /></label>
        {user.protectedRoot && <label className="block text-sm"><input type="checkbox" checked={roleList(roles).includes('superuser')}
          disabled={busy || !selected || target?.protectedRoot} onChange={(event) => {
            const next = roleList(roles).filter((entry) => entry !== 'superuser')
            if (event.target.checked) next.push('superuser')
            setRoles(next.join(', '))
          }} /> Grant SuperUser to selected account</label>}
        <p className="text-xs text-slate-500">Only the initial owner can grant SuperUser. Saving access changes signs the target user out.</p>
        <CollectionGrantsEditor label="Selected user collection permissions" value={(() => {
          try {
            const parsed: unknown = JSON.parse(grants)
            if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {}
            if (!Object.values(parsed).every((actions) => Array.isArray(actions) && actions.every((action: unknown) => typeof action === 'string'))) return {}
            return parsed as CollectionGrants
          } catch { return {} }
        })()} onChange={(next) => setGrants(JSON.stringify(next, null, 2))} />
        <label className="block text-sm">Collection grants (replaces current direct grants)
          <textarea className="block min-h-24 w-full rounded border p-2 font-mono" value={grants} maxLength={8192}
            onChange={(event) => setGrants(event.target.value)} placeholder={'{"orders":["read"]}'} />
        </label>
        <div className="flex flex-wrap gap-3">
          <Button disabled={busy || !selected} onClick={() => void run(async () => {
            if (!user.protectedRoot && roleList(roles).includes('superuser') && !target?.roles?.includes('superuser')) {
              throw new Error('Only the initial protected owner can grant SuperUser')
            }
            await httpClient.patch(`/security/users/${selected}`, { roles: roleList(roles), grants: JSON.parse(grants) }); await loadUsers()
          })}>Replace roles and grants</Button>
          <Button disabled={busy || !selected || target?.protectedRoot} onClick={() => void run(async () => {
            await httpClient.patch(`/security/users/${selected}`, { status: target?.status === 'disabled' ? 'active' : 'disabled' }); await loadUsers()
          })}>{target?.status === 'disabled' ? 'Enable user' : 'Disable user'}</Button>
          <Button disabled={busy || !selected || target?.protectedRoot} onClick={() => void run(async () => {
            const result = await httpClient.post<{ resetToken: string }>(`/security/users/${selected}/reset`, {})
            setFactor(result.resetToken)
          })}>Issue single-use reset token</Button>
        </div>
        {factor && <div className="rounded border border-amber-300 bg-amber-50 p-3 text-sm">
          <p>Deliver through a verified channel. Valid for 15 minutes; hidden here after 60 seconds.</p>
          <output className="block break-all font-mono">{factor}</output>
          <button type="button" className="underline" onClick={() => setFactor('')}>Hide token now</button>
        </div>}
      </section>
      <section className="space-y-3 rounded-xl border bg-white p-5">
        <h2 className="font-semibold">Role definitions</h2>
        <Input aria-label="Role name" placeholder="Role name" value={roleName} onChange={(event) => setRoleName(event.target.value)} />
        <Input aria-label="Parent roles" placeholder="Parent roles, comma-separated" value={parents} onChange={(event) => setParents(event.target.value)} />
        <p className="text-sm text-slate-500">Uses the collection grants above. Updating a role signs out ordinary users so cached results cannot retain revoked access.</p>
        <Button disabled={busy || !roleName} onClick={() => void run(async () => {
          await httpClient.put(`/security/roles/${encodeURIComponent(roleName)}`, { parents: roleList(parents), grants: JSON.parse(grants) })
        })}>Save role</Button>{' '}
        <Button disabled={busy} onClick={() => void run(async () => setDetails(JSON.stringify(await httpClient.get('/security/roles'), null, 2)))}>Inspect roles</Button>{' '}
        <Button disabled={busy} onClick={() => void run(async () => setDetails(JSON.stringify(await httpClient.get('/security/audit'), null, 2)))}>Inspect security audit</Button>
        {details && <pre className="max-h-80 overflow-auto text-xs">{details}</pre>}
      </section>
    </>}
  </div>
}
