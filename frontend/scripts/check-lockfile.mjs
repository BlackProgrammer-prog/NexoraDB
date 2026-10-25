import { readFileSync } from 'node:fs'

const lock = JSON.parse(readFileSync(new URL('../package-lock.json', import.meta.url), 'utf8'))
const errors = []
for (const [name, entry] of Object.entries(lock.packages ?? {})) {
  if (!entry.resolved) continue
  const url = new URL(entry.resolved)
  if (url.origin !== 'https://registry.npmjs.org' || !/^\/(?:@[^/]+\/)?[^/]+\/-\/[^/]+\.tgz$/.test(url.pathname)) {
    errors.push(`${name}: ${entry.resolved}`)
  }
}
if (errors.length) {
  console.error('Non-portable package tarball URLs in lockfile:\n' + errors.join('\n'))
  process.exit(1)
}
console.log('Lockfile tarball URLs use the public npm registry.')
