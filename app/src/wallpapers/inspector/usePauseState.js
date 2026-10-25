import { useCallback, useEffect, useState } from 'react'
import { notifyError } from '../notify'
import { useWallpaperRuntimeStatus } from '../runtimeEvents'

// Paused state for the applied wallpaper. The runtime is the source of truth
// (PAUSED/PLAYING events, including tray-driven toggles); local state only
// makes the button feel instant while the command is in flight.
export function usePauseState(enabled) {
  const status = useWallpaperRuntimeStatus()
  const [paused, setPaused] = useState(false)
  const [pending, setPending] = useState(false)

  useEffect(() => {
    if (!enabled) {
      setPaused(false)
      return
    }
    if (status?.event === 'PAUSED') setPaused(true)
    else if (status?.event === 'PLAYING' || status?.event === 'LOADED') setPaused(false)
  }, [enabled, status])

  const toggle = useCallback(async () => {
    if (!enabled || pending) return
    const bridge = window.paiintDesktop
    if (!bridge?.pauseWallpaper || !bridge?.resumeWallpaper) return

    setPending(true)
    const next = !paused
    try {
      const result = next ? await bridge.pauseWallpaper() : await bridge.resumeWallpaper()
      if (result?.ok === false) {
        notifyError(
          next ? 'Could not pause wallpaper' : 'Could not resume wallpaper',
          result.error,
        )
      } else {
        setPaused(next)
      }
    } catch (err) {
      notifyError('Wallpaper transport failed', err?.message ?? String(err))
    } finally {
      setPending(false)
    }
  }, [enabled, paused, pending])

  return { paused, pending, toggle }
}
