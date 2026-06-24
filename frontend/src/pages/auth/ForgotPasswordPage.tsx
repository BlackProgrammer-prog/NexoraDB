import { Link } from 'react-router-dom'
import { AuthField } from '../../features/auth/components/AuthField'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
import { Button } from '../../shared/components/ui/Button'

export function ForgotPasswordPage() {
  return (
    <AuthLayout
      eyebrow="Password recovery"
      subtitle="Enter your email address and we will send instructions to reset your password."
      title="Reset your password"
    >
      <AuthPanel>
        <form className="space-y-5">
          <AuthField
            autoComplete="email"
            id="email"
            label="Email"
            name="email"
            placeholder="you@example.com"
            type="email"
          />
          <Button className="h-11 w-full" type="submit">
            Send reset link
          </Button>
        </form>
        <p className="mt-6 text-center text-sm text-slate-500">
          Remembered your password?{' '}
          <Link className="font-medium text-green-700 hover:text-green-800" to="/login">
            Back to sign in
          </Link>
        </p>
      </AuthPanel>
    </AuthLayout>
  )
}
