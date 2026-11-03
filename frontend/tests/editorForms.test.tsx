import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { DocumentEditorModal } from '../src/features/documents/components/DocumentEditorModal'
import { NodeEditorModal } from '../src/features/graphs/components/NodeEditorModal'
import { EdgeEditorModal } from '../src/features/graphs/components/EdgeEditorModal'
import type { DocumentRecord } from '../src/features/documents/types/document.types'

const document: DocumentRecord = {
  id: 'one', collectionName: 'users', data: { name: 'First' },
  createdAt: '2026-11-03', updatedAt: '2026-11-03',
}

describe('editor form boundaries', () => {
  it.each(['{', 'null', '[]', '42', 'true', '"text"'])('rejects non-object document JSON: %s', (text) => {
    const save = vi.fn()
    render(<DocumentEditorModal isOpen document={document} onClose={vi.fn()} onSave={save} />)
    fireEvent.change(screen.getByRole('textbox'), { target: { value: text } })
    fireEvent.click(screen.getByRole('button', { name: 'Save document' }))
    expect(save).not.toHaveBeenCalled()
    expect(screen.getByText('Document JSON must be a valid object.')).toBeInTheDocument()
  })

  it('accepts a valid document object, including an empty object', () => {
    const save = vi.fn()
    render(<DocumentEditorModal isOpen onClose={vi.fn()} onSave={save} />)
    fireEvent.click(screen.getByRole('button', { name: 'Save document' }))
    expect(save).toHaveBeenCalledWith({})
  })

  it('resets unsaved text and validation errors when reopened', () => {
    const props = { document, onClose: vi.fn(), onSave: vi.fn() }
    const { rerender } = render(<DocumentEditorModal {...props} isOpen />)
    fireEvent.change(screen.getByRole('textbox'), { target: { value: '{' } })
    fireEvent.click(screen.getByRole('button', { name: 'Save document' }))
    rerender(<DocumentEditorModal {...props} isOpen={false} />)
    expect(screen.queryByRole('textbox')).not.toBeInTheDocument()
    rerender(<DocumentEditorModal {...props} isOpen />)
    expect(JSON.parse((screen.getByRole('textbox') as HTMLTextAreaElement).value)).toEqual(document.data)
    expect(screen.queryByText('Document JSON must be a valid object.')).not.toBeInTheDocument()
  })

  it('resets when switching to another record or collection with the same ID', () => {
    const props = { onClose: vi.fn(), onSave: vi.fn(), isOpen: true }
    const { rerender } = render(<DocumentEditorModal {...props} document={document} />)
    fireEvent.change(screen.getByRole('textbox'), { target: { value: '{"unsaved":true}' } })
    rerender(<DocumentEditorModal {...props} document={{ ...document, collectionName: 'other', data: { next: true } }} />)
    expect(JSON.parse((screen.getByRole('textbox') as HTMLTextAreaElement).value)).toEqual({ next: true })
  })

  it.each(['null', '[]', '42', 'true', '"text"'])('rejects non-object graph node data: %s', (text) => {
    const save = vi.fn()
    render(<NodeEditorModal isOpen node={{ id: 'n', label: 'Node', data: {} }} onClose={vi.fn()} onSave={save} />)
    fireEvent.change(screen.getAllByRole('textbox')[2], { target: { value: text } })
    fireEvent.click(screen.getByRole('button', { name: 'Save node' }))
    expect(save).not.toHaveBeenCalled()
    expect(screen.getByText('Data must be a valid JSON object.')).toBeInTheDocument()
  })

  it.each(['null', '[]', '42', 'true', '"text"'])('rejects non-object graph edge data: %s', (text) => {
    const save = vi.fn()
    render(<EdgeEditorModal isOpen edge={null} nodes={[{ id: 'n', label: 'Node', data: {} }]}
      onClose={vi.fn()} onSave={save} />)
    fireEvent.change(screen.getAllByRole('textbox')[1], { target: { value: text } })
    fireEvent.click(screen.getByRole('button', { name: 'Save edge' }))
    expect(save).not.toHaveBeenCalled()
    expect(screen.getByText('Data must be a valid JSON object.')).toBeInTheDocument()
  })
})
