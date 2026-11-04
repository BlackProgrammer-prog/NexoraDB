import type { Row } from './queryStream'

// Fixed-size ring: evicted row objects are released immediately, not retained
// in an ever-growing array. Encoded bytes are a data budget, not an RSS promise.
export class ResultPreview {
  private rows: (Row | undefined)[]
  private sizes: number[]
  private head = 0
  private count = 0
  private maxRows: number
  private maxBytes: number
  bytes = 0
  total = 0
  constructor(maxRows = 1000, maxBytes = 8_388_608) {
    this.maxRows = maxRows
    this.maxBytes = maxBytes
    this.rows = new Array(maxRows)
    this.sizes = new Array(maxRows).fill(0)
  }
  add(batch: Row[]) {
    for (const row of batch) {
      this.total++
      const size = new TextEncoder().encode(JSON.stringify(row)).byteLength
      while (this.count && (this.count === this.maxRows || this.bytes + size > this.maxBytes)) {
        this.bytes -= this.sizes[this.head]
        this.rows[this.head] = undefined
        this.head = (this.head + 1) % this.maxRows
        this.count--
      }
      if (size > this.maxBytes) continue
      const index = (this.head + this.count) % this.maxRows
      this.rows[index] = row
      this.sizes[index] = size
      this.bytes += size
      this.count++
    }
  }
  snapshot(): Row[] {
    return Array.from({ length: this.count }, (_, index) => this.rows[(this.head + index) % this.maxRows]!)
  }
}
