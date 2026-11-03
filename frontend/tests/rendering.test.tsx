import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { PaginatedResults } from '../src/shared/components/ui/PaginatedResults'
import { JsonPreview } from '../src/shared/components/ui/JsonPreview'
import { QueryResultPanel } from '../src/features/query/components/QueryResultPanel'

function List({ items, size = 50 }: { items: number[]; size?: number }) {
  return <PaginatedResults items={items} pageSize={size}>{(page) => <ul>
    {page.map((item) => <li key={item}>{item}</li>)}
  </ul>}</PaginatedResults>
}

describe('bounded rendering and pagination', () => {
  it('mounts only 50 rows from a 100,000-row dataset', () => {
    render(<List items={Array.from({ length: 100_000 }, (_, i) => i)} />)
    expect(screen.getAllByRole('listitem')).toHaveLength(50)
    expect(screen.getByRole('button', { name: 'Previous' })).toBeDisabled()
    fireEvent.click(screen.getByRole('button', { name: 'Next' }))
    expect(screen.getAllByRole('listitem')).toHaveLength(50)
    expect(screen.getByText('51–100 of 100000')).toBeInTheDocument()
    expect(screen.getAllByRole('listitem')[0]).toHaveTextContent('50')
  })

  it('handles a partial last page and back navigation', () => {
    render(<List items={Array.from({ length: 75 }, (_, i) => i)} />)
    fireEvent.click(screen.getByRole('button', { name: 'Next' }))
    expect(screen.getAllByRole('listitem')).toHaveLength(25)
    expect(screen.getByRole('button', { name: 'Next' })).toBeDisabled()
    fireEvent.click(screen.getByRole('button', { name: 'Previous' }))
    expect(screen.getAllByRole('listitem')).toHaveLength(50)
  })

  it.each([0, 1, 50])('hides navigation when %i results fit on one page', (count) => {
    render(<List items={Array.from({ length: count }, (_, i) => i)} />)
    expect(screen.queryByRole('navigation')).not.toBeInTheDocument()
    expect(screen.queryAllByRole('listitem')).toHaveLength(count)
  })

  it('clamps the current page when data shrinks', () => {
    const { rerender } = render(<List items={Array.from({ length: 200 }, (_, i) => i)} />)
    fireEvent.click(screen.getByRole('button', { name: 'Next' }))
    fireEvent.click(screen.getByRole('button', { name: 'Next' }))
    rerender(<List items={[1, 2]} />)
    expect(screen.getAllByRole('listitem')).toHaveLength(2)
    expect(screen.queryByRole('navigation')).not.toBeInTheDocument()
  })

  it('supports a custom page size', () => {
    render(<List items={Array.from({ length: 50 }, (_, i) => i)} size={20} />)
    expect(screen.getAllByRole('listitem')).toHaveLength(20)
  })

  it('serializes JSON only after expanding, and reuses unchanged previews', () => {
    const read = vi.fn(() => 'payload')
    const value = { get data() { return read() } }
    const { rerender } = render(<JsonPreview value={value} />)
    expect(read).not.toHaveBeenCalled()
    fireEvent.click(screen.getByRole('button', { name: 'Show JSON preview' }))
    expect(read).toHaveBeenCalledTimes(1)
    rerender(<JsonPreview value={value} />)
    expect(read).toHaveBeenCalledTimes(1)
    fireEvent.click(screen.getByRole('button', { name: 'Hide JSON preview' }))
    expect(screen.queryByText(/"payload"/)).not.toBeInTheDocument()
  })

  it('bounds a wide query table and escapes hostile text', () => {
    const columns = Array.from({ length: 100 }, (_, i) => `column${i}`)
    const row = Object.fromEntries(columns.map((key) => [key, '<img src=x onerror="alert(1)">']))
    const { container } = render(<QueryResultPanel error={null} isIdle={false} isLoading={false}
      result={{ columns, rows: Array.from({ length: 2500 }, () => row), raw: {}, executionTimeMs: 1 }} />)
    expect(screen.getAllByRole('columnheader')).toHaveLength(50)
    expect(container.querySelectorAll('tbody tr')).toHaveLength(50)
    expect(container.querySelectorAll('tbody td')).toHaveLength(2500)
    expect(container.querySelector('img')).not.toBeInTheDocument()
    expect(container.querySelector('pre')).not.toBeInTheDocument()
  })

  it('shows an error without rendering stale results', () => {
    render(<QueryResultPanel error={new Error('Query denied')} isIdle={false} isLoading={false}
      result={{ columns: ['secret'], rows: [{ secret: 'stale' }], raw: {}, executionTimeMs: 1 }} />)
    expect(screen.getByText('Query denied')).toBeInTheDocument()
    expect(screen.queryByText('stale')).not.toBeInTheDocument()
  })
})
