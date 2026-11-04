import { useEffect, useRef } from 'react'
import { basicSetup, EditorView } from 'codemirror'
import { EditorState } from '@codemirror/state'
import { keymap } from '@codemirror/view'
import { indentWithTab } from '@codemirror/commands'
import { lintGutter, setDiagnostics } from '@codemirror/lint'
import { nexoraLanguage } from './nexoraLanguage'
import { editorDiagnostic } from './diagnostics'
import type { QueryDiagnostic } from '../types/query.types'

interface Props {
  value: string
  onChange: (query: string) => void
  onRun: (query: string) => void
  diagnostics: QueryDiagnostic[]
}
export function CodeEditor({ value, onChange, onRun, diagnostics }: Props) {
  const host = useRef<HTMLDivElement | null>(null)
  const editor = useRef<EditorView | null>(null)
  const callbacks = useRef({ onChange, onRun, value })
  useEffect(() => { callbacks.current = { onChange, onRun, value } }, [onChange, onRun, value])
  useEffect(() => {
    const view = new EditorView({
      parent: host.current!,
      state: EditorState.create({ doc: callbacks.current.value, extensions: [
        basicSetup, nexoraLanguage, lintGutter(),
        EditorView.contentAttributes.of({ 'aria-label': 'Query', role: 'textbox', 'aria-multiline': 'true', spellcheck: 'false' }),
        keymap.of([{ key: 'Mod-Enter', run: (view) => { callbacks.current.onRun(view.state.doc.toString()); return true } }, indentWithTab]),
        EditorState.transactionFilter.of((transaction) => transaction.newDoc.length > 200_000 ? [] : transaction),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) callbacks.current.onChange(update.state.doc.toString())
        }),
        EditorView.theme({ '&': { fontSize: '14px' }, '.cm-scroller': { maxHeight: '24rem', overflow: 'auto', fontFamily: 'ui-monospace, monospace' },
          '.cm-content': { minHeight: '12rem' }, '.cm-gutters': { backgroundColor: '#f8fafc' } }),
      ] }),
    })
    editor.current = view
    return () => { view.destroy(); editor.current = null }
  }, [])
  useEffect(() => {
    const view = editor.current
    if (view && view.state.doc.toString() !== value) {
      view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: value } })
    }
  }, [value])
  useEffect(() => {
    const view = editor.current
    if (view) view.dispatch(setDiagnostics(view.state, diagnostics.filter((item) => item.line != null && item.column != null).map((item) => editorDiagnostic(value, item))))
  }, [diagnostics, value])
  return <div className="overflow-hidden rounded-md border border-slate-300 bg-white" ref={host} />
}
