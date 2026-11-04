import { describe, expect, it } from 'vitest'
import { ResultPreview } from '../src/features/query/services/resultPreview'

describe('stream preview ring buffer', () => {
  it('keeps the newest rows in order after wrapping repeatedly', () => {
    const preview = new ResultPreview(3)
    preview.add(Array.from({ length: 10 }, (_, value) => ({ value })))
    expect(preview.total).toBe(10)
    expect(preview.snapshot()).toEqual([{ value: 7 }, { value: 8 }, { value: 9 }])
  })
  it('evicts by UTF-8 bytes even before reaching the row-count cap', () => {
    const row = { text: 'ی'.repeat(10) }
    const size = new TextEncoder().encode(JSON.stringify(row)).byteLength
    const preview = new ResultPreview(1000, size * 2)
    preview.add([row, row, row])
    expect(preview.snapshot()).toHaveLength(2)
    expect(preview.bytes).toBe(size * 2)
    expect(preview.total).toBe(3)
  })
  it('never retains a row that alone exceeds the data budget', () => {
    const preview = new ResultPreview(10, 50)
    preview.add([{ data: 'x'.repeat(100) }, { value: 1 }])
    expect(preview.total).toBe(2)
    expect(preview.snapshot()).toEqual([{ value: 1 }])
    expect(preview.bytes).toBeLessThanOrEqual(50)
  })
})
