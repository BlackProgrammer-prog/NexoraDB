import { useState, type FormEvent } from 'react'
import { Link } from 'react-router-dom'
import { authApi } from '../../features/auth/services/authApi'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { Button } from '../../shared/components/ui/Button'
export function ForgotPasswordPage() {
  const [token, setToken] = useState('')
  const [password, setPassword] = useState('')
  const [confirmation, setConfirmation] = useState('')
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)
  async function submit(event: FormEvent) {
    event.preventDefault()
    if (password !== confirmation) { setMessage('Passwords do not match'); return }
    setBusy(true)
    try { const result = await authApi.resetPassword(token, password); setMessage(result.message) }
    catch (error) { setMessage(error instanceof Error ? error.message : 'Reset failed') }
    finally { setToken(''); setPassword(''); setConfirmation(''); setBusy(false) }
  }
  return <AuthLayout eyebrow="Password recovery" title="Set a new password"
    subtitle="Ask an administrator for a single-use reset token. Tokens expire after 15 minutes.">
    <AuthPanel>
      <form className="space-y-5" onSubmit={(event) => void submit(event)}>
        <AuthField label="Reset token" id="resetToken" name="resetToken" type="password"
          value={token} onChange={(event) => setToken(event.target.value)} autoComplete="off" required />
        <AuthField label="New password (at least 15 characters)" id="newPassword" name="newPassword" type="password"
          value={password} onChange={(event) => setPassword(event.target.value)} autoComplete="new-password" required minLength={15} maxLength={1024} />
        <AuthField label="Confirm new password" id="confirmation" name="confirmation" type="password"
          value={confirmation} onChange={(event) => setConfirmation(event.target.value)} autoComplete="new-password" required />
        <Button disabled={busy} type="submit">{busy ? 'Resetting...' : 'Reset password'}</Button>
        <p role="status" className="text-sm">{message}</p>
      </form>
      <p className="mt-6 text-sm leading-6 text-slate-600">Owner recovery requires stopping the database
        and entering an enrolled recovery factor on the host. Owner recovery factors are never entered here.</p>
      <p className="mt-5"><Link className="text-green-700" to="/login">Back to sign in</Link></p>
    </AuthPanel>
  </AuthLayout>
}
