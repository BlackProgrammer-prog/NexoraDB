import { render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { DocumentList } from '../src/features/documents/components/DocumentList'
import { useDocuments } from '../src/features/documents/hooks/useDocuments'
import type { DocumentRecord } from '../src/features/documents/types/document.types'

vi.mock('../src/features/documents/hooks/useDocuments', () => ({ useDocuments: vi.fn() }))
const document: DocumentRecord = {
  id: 'controlled', collectionName: 'users', data: {}, createdAt: '2026-11-03', updatedAt: '2026-11-03',
}

describe('single owner for document fetching', () => {
  it('uses parent results without creating a second data request', () => {
    render(<DocumentList collectionName="users" documents={[document]} isLoading={false} />)
    expect(screen.getByRole('heading', { name: 'controlled' })).toBeInTheDocument()
    expect(useDocuments).not.toHaveBeenCalled()
  })

  it('does not fall back to fetching when the controlled result is empty', () => {
    render(<DocumentList collectionName="users" documents={null} isLoading={false} />)
    expect(screen.getByText('No documents found')).toBeInTheDocument()
    expect(useDocuments).not.toHaveBeenCalled()
  })

  it('fetches internally only when no controlled result is provided', () => {
    vi.mocked(useDocuments).mockReturnValue({ data: [document], error: null, isLoading: false, refetch: vi.fn() })
    render(<DocumentList collectionName="users" />)
    expect(useDocuments).toHaveBeenCalledWith('users')
    expect(screen.getByRole('heading', { name: 'controlled' })).toBeInTheDocument()
  })
})
