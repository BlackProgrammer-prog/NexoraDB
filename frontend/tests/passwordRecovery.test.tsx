import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { beforeEach, expect, it, vi } from 'vitest'
import { ForgotPasswordPage } from '../src/pages/auth/ForgotPasswordPage'
import { RegisterPage } from '../src/pages/auth/RegisterPage'
import { authApi } from '../src/features/auth/services/authApi'

vi.mock('../src/features/auth/services/authApi', () => ({ authApi: { resetPassword: vi.fn() } }))
beforeEach(() => vi.mocked(authApi.resetPassword).mockReset())

it('redeems a reset factor and clears the credential inputs', async () => {
  vi.mocked(authApi.resetPassword).mockResolvedValue({ message: 'Reset complete' })
  render(<MemoryRouter><ForgotPasswordPage /></MemoryRouter>)
  fireEvent.change(screen.getByLabelText('Reset token'), { target: { value: 'private-reset-factor' } })
  fireEvent.change(screen.getByLabelText(/New password/), { target: { value: 'new private password' } })
  fireEvent.change(screen.getByLabelText('Confirm new password'), { target: { value: 'new private password' } })
  fireEvent.click(screen.getByRole('button', { name: 'Reset password' }))
  await waitFor(() => expect(authApi.resetPassword).toHaveBeenCalledWith('private-reset-factor', 'new private password'))
  await waitFor(() => expect(screen.getByLabelText('Reset token')).toHaveValue(''))
  expect(screen.getByLabelText(/New password/)).toHaveValue('')
  expect(screen.getByLabelText('Confirm new password')).toHaveValue('')
  expect(screen.queryByLabelText(/Email/)).toBeNull()
})

it('provides local guidance without an unauthenticated registration form', () => {
  render(<MemoryRouter><RegisterPage /></MemoryRouter>)
  expect(screen.getByText(/Public registration is disabled/)).toBeVisible()
  expect(screen.queryByRole('textbox')).toBeNull()
  expect(screen.queryByRole('button', { name: /Create account/i })).toBeNull()
})
