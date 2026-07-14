import { Link } from 'react-router-dom'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { Button } from '../../shared/components/ui/Button'

export function RegisterPage() {
  return (
    <AuthLayout
      eyebrow="Create account"
      subtitle="Set up your profile and choose a secure password for NexoraDB."
      title="Start your workspace"
    >
      <AuthPanel>
        <form className="space-y-5">
          <div className="grid gap-5 sm:grid-cols-2">
            <AuthField
              autoComplete="given-name"
              id="firstName"
              label="First name"
              name="firstName"
              placeholder="John"
              type="text"
            />
            <AuthField
              autoComplete="family-name"
              id="lastName"
              label="Last name"
              name="lastName"
              placeholder="Carter"
              type="text"
            />
          </div>
          <AuthField
            autoComplete="email"
            id="email"
            label="Email"
            name="email"
            placeholder="you@example.com"
            type="email"
          />
          <AuthField
            autoComplete="new-password"
            id="password"
            label="Password"
            name="password"
            placeholder="Create a password"
            type="password"
          />
          <AuthField
            autoComplete="new-password"
            id="confirmPassword"
            label="Confirm password"
            name="confirmPassword"
            placeholder="Repeat your password"
            type="password"
          />
          <Button className="h-11 w-full" type="submit">
            Create account
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
