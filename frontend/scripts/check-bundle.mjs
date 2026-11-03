import { readFileSync } from 'node:fs'
import { gzipSync } from 'node:zlib'

const output = new URL('../../NexoraDB/src/nexoradb/dashboard/static/', import.meta.url)
const html = readFileSync(new URL('index.html', output), 'utf8')
const entry = html.match(/src="\/admin\/(assets\/index-[^"]+\.js)"/)?.[1]
if (!entry) throw new Error('Production entry was not found. Run npm run build first.')
const bytes = readFileSync(new URL(entry, output))
const compressed = gzipSync(bytes).length
console.log(`Entry: ${bytes.length} bytes; gzip: ${compressed} bytes`)
if (bytes.length > 300 * 1024 || compressed > 100 * 1024) {
  throw new Error('Entry exceeds the 300 KiB / 100 KiB gzip budget. Check for eager graph/chart imports.')
}
