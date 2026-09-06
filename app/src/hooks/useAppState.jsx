import { useCallback, useEffect, useState } from 'react'

// Reads/writes a single persisted UI preference via the AppStateManager
// (main process, <userData>/app-state.json). Values load asynchronously on
// mount; until then the defaultValue is used.
export function useAppState(keyPath, defaultValue) {
  const [value, setValue] = useState(defaultValue)

  useEffect(() => {
    let cancelled = false
    window.rexDesktop?.appState
      ?.get(keyPath)
      .then((v) => {
        if (!cancelled && v !== undefined && v !== null) setValue(v)
      })
      .catch(() => {})
    return () => {
      cancelled = true
    }
  }, [keyPath])

  const updateValue = useCallback(
    (newValue) => {
      setValue(newValue)
      window.rexDesktop?.appState?.set(keyPath, newValue)
    },
    [keyPath]
  )

  return [value, updateValue]
}