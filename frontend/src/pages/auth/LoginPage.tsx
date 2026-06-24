import { Link } from 'react-router-dom'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { Button } from '../../shared/components/ui/Button'

export function LoginPage() {
  return (
    <AuthLayout
      eyebrow="Welcome back"
      subtitle="Enter your username and password to continue to your workspace."
      title="Sign in to your account"
    >
      <AuthPanel>
        <form className="space-y-5">
          <AuthField
            autoComplete="username"
            id="username"
            label="Username"
            name="username"
            placeholder="Enter your username"
            type="text"
          />
          <AuthField
            autoComplete="current-password"
            id="password"
            label="Password"
            name="password"
            placeholder="Enter your password"
            type="password"
          />
          <div className="flex items-center justify-end">
            <Link
              className="text-sm font-medium text-green-700 transition hover:text-green-800"
              to="/forgot-password"
            >
              Forgot password?
            </Link>
          </div>
          <Button className="h-11 w-full" type="submit">
            Sign in
          </Button>
        </form>
        <p className="mt-6 text-center text-sm text-slate-500">
          Do not have an account?{' '}
          <Link className="font-medium text-green-700 hover:text-green-800" to="/register">
            Create one
          </Link>
        </p>
      </AuthPanel>
    </AuthLayout>
  )
}
