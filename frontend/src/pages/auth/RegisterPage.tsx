import { useEffect, useState, type FormEvent } from 'react'
import { Link, useNavigate } from 'react-router-dom'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { useAuth } from '../../features/auth/context/AuthContext'
import { authApi } from '../../features/auth/services/authApi'
import { Button } from '../../shared/components/ui/Button'

export function RegisterPage() {
  const navigate = useNavigate()
  const { isAuthenticated, isInitializing, registerRootAdmin } = useAuth()
  const [firstName, setFirstName] = useState('')
  const [lastName, setLastName] = useState('')
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [confirmPassword, setConfirmPassword] = useState('')
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [isCheckingSetup, setIsCheckingSetup] = useState(true)
  const [isSubmitting, setIsSubmitting] = useState(false)

  useEffect(() => {
    let isActive = true

    async function checkSetupState() {
      try {
        const setupState = await authApi.getSetupState()

        if (isActive && !setupState.needsSetup) {
          navigate('/login', { replace: true })
        }
      } catch (error) {
        if (isActive) {
          setErrorMessage(
            error instanceof Error ? error.message : 'Could not check setup state',
          )
        }
      } finally {
        if (isActive) {
          setIsCheckingSetup(false)
        }
      }
    }

    void checkSetupState()

    return () => {
      isActive = false
    }
  }, [navigate])

  useEffect(() => {
    if (!isInitializing && isAuthenticated) {
      navigate('/dashboard', { replace: true })
    }
  }, [isAuthenticated, isInitializing, navigate])

  async function handleSubmit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    setErrorMessage(null)

    if (password !== confirmPassword) {
      setErrorMessage('Password and confirmation do not match')
      return
    }

    setIsSubmitting(true)

    try {
      await registerRootAdmin({
        confirmPassword,
        email,
        firstName,
        lastName,
        password,
      })
      navigate('/dashboard', { replace: true })
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : 'Could not create admin account')
    } finally {
      setIsSubmitting(false)
    }
  }

  return (
    <AuthLayout
      eyebrow="Create account"
      subtitle="Set up your profile and choose a secure password for NexoraDB."
      title="Start your workspace"
    >
      <AuthPanel>
        <form className="space-y-5" onSubmit={(event) => void handleSubmit(event)}>
          {errorMessage ? (
            <div className="rounded-md border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
              {errorMessage}
            </div>
          ) : null}
          <div className="rounded-md border border-green-100 bg-green-50 px-3 py-2 text-sm text-green-800">
            This setup screen is available only before the first root admin account is
            created.
          </div>
          <div className="grid gap-5 sm:grid-cols-2">
            <AuthField
              autoComplete="given-name"
              disabled={isCheckingSetup || isSubmitting}
              id="firstName"
              label="First name"
              name="firstName"
              onChange={(event) => setFirstName(event.target.value)}
              placeholder="John"
              required
              type="text"
              value={firstName}
            />
            <AuthField
              autoComplete="family-name"
              disabled={isCheckingSetup || isSubmitting}
              id="lastName"
              label="Last name"
              name="lastName"
              onChange={(event) => setLastName(event.target.value)}
              placeholder="Carter"
              required
              type="text"
              value={lastName}
            />
          </div>
          <AuthField
            autoComplete="email"
            disabled={isCheckingSetup || isSubmitting}
            id="email"
            label="Email"
            name="email"
            onChange={(event) => setEmail(event.target.value)}
            placeholder="you@example.com"
            required
            type="email"
            value={email}
          />
          <AuthField
            autoComplete="new-password"
            disabled={isCheckingSetup || isSubmitting}
            id="password"
            label="Password"
            name="password"
            onChange={(event) => setPassword(event.target.value)}
            placeholder="Create a password"
            required
            type="password"
            value={password}
          />
          <AuthField
            autoComplete="new-password"
            disabled={isCheckingSetup || isSubmitting}
            id="confirmPassword"
            label="Confirm password"
            name="confirmPassword"
            onChange={(event) => setConfirmPassword(event.target.value)}
            placeholder="Repeat your password"
            required
            type="password"
            value={confirmPassword}
          />
          <p className="text-xs leading-5 text-slate-500">
            Password must be at least 12 characters and include lowercase, uppercase,
            and a number.
          </p>
          <Button
            className="h-11 w-full"
            disabled={isCheckingSetup || isSubmitting}
            type="submit"
          >
            {isSubmitting ? 'Creating account...' : 'Create account'}
          </Button>
        </form>
        <p className="mt-6 text-center text-sm text-slate-500">
          Already have an account?{' '}
          <Link className="font-medium text-green-700 hover:text-green-800" to="/login">
            Sign in
          </Link>
        </p>
      </AuthPanel>
    </AuthLayout>
  )
}
