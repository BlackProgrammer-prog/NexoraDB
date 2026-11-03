import { describe, expect, it, vi } from 'vitest'
import { previewJson } from '../src/shared/utils/previewJson'

describe('bounded JSON preview', () => {
  it('preserves small objects including Unicode and JSON primitives', () => {
    const data = { title: 'دیتابیس', values: [1, true, null] }
    expect(JSON.parse(previewJson(data))).toEqual(data)
  })

  it('visits only a bounded prefix of a million-element array', () => {
    const read = vi.fn(() => 'item')
    const values = new Proxy(new Array(1_000_000), {
      get(target, key, receiver) {
        return typeof key === 'string' && /^\d+$/.test(key) ? read() : Reflect.get(target, key, receiver)
      },
    })
    const result = JSON.parse(previewJson(values))
    expect(read.mock.calls.length).toBeLessThanOrEqual(100)
    expect(result).toContain('[more items omitted]')
  })

  it('does not read every field of a wide object', () => {
    const read = vi.fn(() => 1)
    const data = Object.fromEntries(Array.from({ length: 10_000 }, (_, i) => [i, 0]))
    for (const key of Object.keys(data)) Object.defineProperty(data, key, { get: read, enumerable: true })
    expect(previewJson(data)).toContain('more fields omitted')
    expect(read.mock.calls.length).toBeLessThanOrEqual(100)
  })

  it('stops descending before deeply nested data can exhaust the stack', () => {
    let data: unknown = { final: 'unreachable' }
    for (let i = 0; i < 1000; i++) data = { nested: data }
    expect(previewJson(data)).toContain('[preview limit]')
    expect(previewJson(data)).not.toContain('unreachable')
  })

  it('handles cycles and bigint values', () => {
    const data: { value: bigint; self?: unknown } = { value: 42n }
    data.self = data
    expect(JSON.parse(previewJson(data))).toEqual({ value: '42', self: '[circular]' })
  })

  it('truncates large strings before JSON serialization', () => {
    const result = previewJson({ data: 'x'.repeat(1_000_000) })
    expect(result.length).toBeLessThan(2100)
    expect(result).toContain('[truncated]')
  })

  it('handles __proto__ as data without polluting Object.prototype', () => {
    const result = JSON.parse(previewJson(JSON.parse('{"__proto__":{"polluted":true}}')))
    expect(Object.hasOwn(result, '__proto__')).toBe(true)
    expect(Object.hasOwn(Object.prototype, 'polluted')).toBe(false)
  })
})
