import { Component, type PropsWithChildren } from 'react'

export class PageErrorBoundary extends Component<PropsWithChildren, { failed: boolean }> {
  state = { failed: false }

  static getDerivedStateFromError() { return { failed: true } }

  render() {
    if (this.state.failed) {
      return <div role="alert" className="m-6 rounded-lg border border-red-200 bg-white p-6 text-slate-800">
        <h1 className="font-semibold">Could not load the console page</h1>
        <p className="my-3 text-sm">Check your connection and reload. Unsaved edits will be lost.</p>
        <button type="button" className="rounded border px-3 py-2" onClick={() => window.location.reload()}>Reload console</button>
      </div>
    }
    return this.props.children
  }
}
