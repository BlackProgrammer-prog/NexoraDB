import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { beforeEach, expect, it, vi } from 'vitest'
import { SecurityPage } from '../src/pages/SecurityPage'
import { useAuth } from '../src/features/auth/context/useAuth'
import { httpClient } from '../src/api/client/httpClient'
import { admin } from './helpers'

vi.mock('../src/features/auth/context/useAuth', () => ({ useAuth: vi.fn() }))
vi.mock('../src/api/client/httpClient', () => ({ httpClient: { post: vi.fn(), get: vi.fn(), patch: vi.fn(), put: vi.fn() } }))

function session(role: 'admin' | 'application', owner = false) {
  vi.mocked(useAuth).mockReturnValue({ user: { ...admin, role, protectedRoot: owner }, logout: vi.fn() } as unknown as ReturnType<typeof useAuth>)
}
beforeEach(() => {
  vi.clearAllMocks()
  vi.mocked(httpClient.post).mockResolvedValue({})
  vi.mocked(httpClient.get).mockResolvedValue([])
})

it('hides user administration from ordinary users', () => {
  session('application')
  render(<SecurityPage />)
  expect(screen.queryByRole('heading', { name: 'Create a user' })).toBeNull()
  expect(screen.getByRole('button', { name: 'Change password and sign out' })).toBeVisible()
})

it('creates a user with an initial password, grants and owner-selected SuperUser in one request', async () => {
  session('admin', true)
  render(<SecurityPage />)
  fireEvent.change(screen.getByLabelText('New username'), { target: { value: 'operator' } })
  fireEvent.change(screen.getByLabelText('Initial password'), { target: { value: 'private initial password' } })
  fireEvent.change(screen.getByLabelText('Initial collection permissions collection'), { target: { value: 'posts' } })
  fireEvent.click(screen.getAllByRole('button', { name: 'Add collection grant' })[0])
  fireEvent.click(screen.getByRole('checkbox', { name: 'Grant SuperUser to the new account' }))
  fireEvent.click(screen.getByRole('button', { name: 'Create user' }))
  await waitFor(() => expect(httpClient.post).toHaveBeenCalledWith('/security/users', {
    username: 'operator', password: 'private initial password', roles: ['superuser'], grants: { posts: ['read'] },
  }))
  await waitFor(() => expect(screen.getByLabelText('Initial password')).toHaveValue(''))
  expect(screen.getByRole('status')).not.toHaveTextContent('private initial password')
})

it('lets delegated SuperUsers create users while hiding owner-only promotion', async () => {
  session('admin')
  render(<SecurityPage />)
  expect(screen.getByRole('heading', { name: 'Create a user' })).toBeVisible()
  expect(screen.queryByRole('checkbox', { name: /Grant SuperUser/ })).toBeNull()
  fireEvent.change(screen.getByLabelText('New username'), { target: { value: 'reader' } })
  fireEvent.change(screen.getByLabelText('Initial password'), { target: { value: 'private initial password' } })
  fireEvent.change(screen.getByLabelText('Initial roles (comma-separated)'), { target: { value: 'superuser' } })
  fireEvent.click(screen.getByRole('button', { name: 'Create user' }))
  await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Only the initial protected owner'))
  expect(httpClient.post).not.toHaveBeenCalled()
  expect(screen.getByLabelText('Initial password')).toHaveValue('')
})
