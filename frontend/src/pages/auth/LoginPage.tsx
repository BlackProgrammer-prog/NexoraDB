import { useEffect, useState, type FormEvent } from 'react'
import { Link, useLocation, useNavigate } from 'react-router-dom'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { useAuth } from '../../features/auth/context/AuthContext'
import { authApi } from '../../features/auth/services/authApi'
import { Button } from '../../shared/components/ui/Button'

interface LoginLocationState {
  from?: {
    pathname?: string
  }
}

export function LoginPage() {
  const navigate = useNavigate()
  const location = useLocation()
  const { isAuthenticated, isInitializing, login } = useAuth()
  const [username, setUsername] = useState('root')
  const [password, setPassword] = useState('')
  const [errorMessage, setErrorMessage] = useState<string | null>(null)
  const [isCheckingSetup, setIsCheckingSetup] = useState(true)
  const [isSubmitting, setIsSubmitting] = useState(false)

  const redirectTo =
    (location.state as LoginLocationState | null)?.from?.pathname ?? '/dashboard'

  useEffect(() => {
    let isActive = true

    async function checkSetupState() {
      try {
        const setupState = await authApi.getSetupState()

        if (isActive && setupState.needsSetup) {
          navigate('/register', { replace: true })
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
      navigate(redirectTo, { replace: true })
    }
  }, [isAuthenticated, isInitializing, navigate, redirectTo])

  async function handleSubmit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    setErrorMessage(null)
    setIsSubmitting(true)

    try {
      await login({ password, username })
      navigate(redirectTo, { replace: true })
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : 'Sign in failed')
    } finally {
      setIsSubmitting(false)
    }
  }

  return (
    <AuthLayout
      eyebrow="Welcome back"
      subtitle="Enter your username and password to continue to your workspace."
      title="Sign in to your account"
    >
      <AuthPanel>
        <form
          className="space-y-5"
          onSubmit={(event) => void handleSubmit(event)}
        >
          {errorMessage ? (
            <div className="rounded-md border border-red-200 bg-red-50 px-3 py-2 text-sm text-red-700">
              {errorMessage}
            </div>
          ) : null}
          <AuthField
            autoComplete="username"
            disabled={isCheckingSetup || isSubmitting}
            id="username"
            label="Username"
            name="username"
            onChange={(event) => setUsername(event.target.value)}
            placeholder="Enter your username"
            required
            type="text"
            value={username}
          />
          <AuthField
            autoComplete="current-password"
            disabled={isCheckingSetup || isSubmitting}
            id="password"
            label="Password"
            name="password"
            onChange={(event) => setPassword(event.target.value)}
            placeholder="Enter your password"
            required
            type="password"
            value={password}
          />
          <div className="flex items-center justify-end">
            <Link
              className="text-sm font-medium text-green-700 transition hover:text-green-800"
              to="/forgot-password"
            >
              Forgot password?
            </Link>
          </div>
          <Button
            className="h-11 w-full"
            disabled={isCheckingSetup || isSubmitting}
            type="submit"
          >
            {isSubmitting ? 'Signing in...' : 'Sign in'}
          </Button>
        </form>
        <p className="mt-6 text-center text-sm text-slate-500">
          Admin setup is only available before the root account exists.
        </p>
      </AuthPanel>
    </AuthLayout>
  )
}
