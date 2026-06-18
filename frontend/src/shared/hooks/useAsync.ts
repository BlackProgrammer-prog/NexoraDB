import { useCallback, useEffect, useState } from 'react'

interface AsyncState<TData> {
  data: TData | null
  error: Error | null
  isLoading: boolean
}

export function useAsync<TData>(asyncFunction: () => Promise<TData>) {
  const [state, setState] = useState<AsyncState<TData>>({
    data: null,
    error: null,
    isLoading: true,
  })

  const execute = useCallback(async () => {
    setState((current) => ({ ...current, error: null, isLoading: true }))

    try {
      const data = await asyncFunction()
      setState({ data, error: null, isLoading: false })
    } catch (error) {
      setState({
        data: null,
        error: error instanceof Error ? error : new Error('Unexpected error'),
        isLoading: false,
      })
    }
  }, [asyncFunction])

  useEffect(() => {
    let isActive = true

    async function load() {
      try {
        const data = await asyncFunction()

        if (isActive) {
          setState({ data, error: null, isLoading: false })
        }
      } catch (error) {
        if (isActive) {
          setState({
            data: null,
            error: error instanceof Error ? error : new Error('Unexpected error'),
            isLoading: false,
          })
        }
      }
    }

    void load()

    return () => {
      isActive = false
    }
  }, [asyncFunction])

  return { ...state, refetch: execute }
}
