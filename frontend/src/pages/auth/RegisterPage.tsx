import { Link } from 'react-router-dom'
import { AuthLayout } from '../../features/auth/components/AuthLayout'
import { AuthPanel } from '../../features/auth/components/AuthPanel'
export function RegisterPage() {
  return <AuthLayout eyebrow="Local setup" title="Ask your operator to enroll the owner"
    subtitle="The first owner account is created on the database host. Public registration is disabled.">
    <AuthPanel>
      <p className="text-sm leading-6 text-slate-600">The operator chooses the owner username and password
        using local setup and saves the offline recovery factors. Existing databases use the explicit migration
        workflow. Additional accounts are created by an authorized administrator.</p>
      <p className="mt-6"><Link className="font-medium text-green-700" to="/login">Back to sign in</Link></p>
    </AuthPanel>
  </AuthLayout>
}
