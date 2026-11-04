import { StreamLanguage, type StringStream } from '@codemirror/language'

const keywords = new Set(`SELECT FROM WHERE LIMIT SKIP INSERT INTO VALUES UPDATE SET DELETE
CREATE DROP ALTER COLLECTION INDEX UNIQUE ON SHOW COLLECTIONS EXISTS COUNT DESCRIBE USE GRAPH
GRAPHS LIVE STATIC DIRECTED UNDIRECTED HETEROGENEOUS MAP NODE EDGE KEY PROPERTIES SOURCE TARGET
AS BUILD REFRESH RENDER TRAVERSE OUT IN BOTH DEPTH RUN LOCK JOB WITH BEGIN COMMIT ROLLBACK
TRANSACTION AND OR NOT IS NULL TRUE FALSE LOOKUP JOIN INNER LEFT SYSTEM STATUS INFO RETURNS TOP
FOREIGN REFERENCES RESTRICT`.split(/\s+/))

interface TokenState { quote: string | null; block: boolean }
function stringToken(stream: StringStream, state: TokenState) {
  while (!stream.eol()) {
    const char = stream.next()
    if (char === '\\') stream.next()
    else if (char === state.quote) { state.quote = null; break }
  }
  return 'string'
}
function commentToken(stream: StringStream, state: TokenState) {
  while (!stream.eol()) {
    if (stream.match('*/')) { state.block = false; break }
    stream.next()
  }
  return 'comment'
}

// Lexical highlighting follows NexoraQL's strings/comments/parameters. The
// server's actual parser, rather than a SQL approximation, owns diagnostics.
export const nexoraLanguage = StreamLanguage.define<TokenState>({
  startState: () => ({ quote: null, block: false }),
  token(stream, state) {
    if (state.block) return commentToken(stream, state)
    if (state.quote) return stringToken(stream, state)
    if (stream.eatSpace()) return null
    if (stream.match('--')) { stream.skipToEnd(); return 'comment' }
    if (stream.match('/*')) { state.block = true; return commentToken(stream, state) }
    const char = stream.peek()
    if (char === "'" || char === '"') { state.quote = stream.next()!; return stringToken(stream, state) }
    if (stream.match(/^\$[A-Za-z_]\w*/)) return 'variableName.special'
    if (stream.match(/^-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/)) return 'number'
    const word = stream.match(/^[A-Za-z_]\w*/)
    if (word && typeof word !== 'boolean') return keywords.has(word[0].toUpperCase()) ? 'keyword' : 'variableName'
    stream.next()
    return /[=<>!+*/%-]/.test(char ?? '') ? 'operator' : 'punctuation'
  },
  languageData: { commentTokens: { line: '--', block: { open: '/*', close: '*/' } } },
})
