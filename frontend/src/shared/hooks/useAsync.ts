import { useCallback, useEffect, useRef, useState } from 'react'

interface AsyncState<T> {
  data: T | null
  error: Error | null
  isLoading: boolean
}

// One owner per request: superseded fetches cannot overwrite newer data.
export function useAsync<T>(asyncFunction: (signal: AbortSignal) => Promise<T>) {
  const [state, setState] = useState<AsyncState<T>>({ data: null, error: null, isLoading: true })
  const [source, setSource] = useState(() => asyncFunction)
  const active = useRef<AbortController | null>(null)
  const owner = useRef<(() => Promise<void>) | null>(null)
  if (source !== asyncFunction) {
    setSource(() => asyncFunction)
    setState({ data: null, error: null, isLoading: true })
  }

  const load = useCallback(async () => {
    active.current?.abort()
    const controller = new AbortController()
    active.current = controller
    let task: Promise<T>
    try {
      task = asyncFunction(controller.signal)
    } catch (error) {
      task = Promise.reject(error)
    }
    return task.then(
      (data) => {
        if (!controller.signal.aborted) setState({ data, error: null, isLoading: false })
      },
      (error: unknown) => {
        if (!controller.signal.aborted) {
          setState({ data: null, error: error instanceof Error ? error : new Error('Unexpected error'), isLoading: false })
        }
      },
    )
  }, [asyncFunction])

  const refetch = useCallback(() => {
    if (owner.current !== load) return Promise.resolve()
    setState((current) => ({ ...current, error: null, isLoading: true }))
    return load()
  }, [load])

  useEffect(() => {
    owner.current = load
    void load()
    return () => {
      owner.current = null
      active.current?.abort()
    }
  }, [load])

  return { ...state, refetch }
}
