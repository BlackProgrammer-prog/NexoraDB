// Budget work before serializing: slicing a full JSON.stringify result is too late.
export function previewJson(value: unknown): string {
  let remaining = 500
  const seen = new WeakSet<object>()
  function visit(item: unknown, depth: number): unknown {
    if (--remaining < 0 || depth > 8) return '[preview limit]'
    if (typeof item === 'string') return item.length > 2000 ? item.slice(0, 2000) + '… [truncated]' : item
    if (typeof item === 'bigint') return String(item)
    if (item === null || typeof item !== 'object') return item
    if (seen.has(item)) return '[circular]'
    seen.add(item)
    if (Array.isArray(item)) {
      const output = []
      for (let i = 0; i < item.length && remaining > 0 && i < 100; i++) output.push(visit(item[i], depth + 1))
      if (output.length < item.length) output.push('[more items omitted]')
      return output
    }
    const output: Record<string, unknown> = Object.create(null)
    let count = 0
    for (const key in item) {
      if (!Object.hasOwn(item, key)) continue
      if (remaining <= 0 || count++ >= 100) { output['[preview limit]'] = 'more fields omitted'; break }
      output[key.slice(0, 200)] = visit((item as Record<string, unknown>)[key], depth + 1)
    }
    return output
  }
  return JSON.stringify(visit(value, 0), null, 2) ?? 'null'
}
