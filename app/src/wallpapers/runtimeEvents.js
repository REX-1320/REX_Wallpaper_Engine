import { useEffect, useState } from 'react'

import { notifyError } from './notify'

// ─── Runtime event bridge ──────────────────────────────────────────────────
// Subscribes once to the Electron bridge (window.rexDesktop.onWallpaperEvent)
// and fans events out to React hooks. Events: READY, LOADED, PLAYING,
// PAUSED, STOPPED, UNLOADED, ERROR — each `{ event, data }`, plus supervisor
// recovery events (recovery-start/ok/gave-up). ERROR and recovery-gave-up
// surface as user-facing toasts (P1 error-to-toast path); the last valid
// wallpaper stays on screen (failed LOADs never clear it).

let latest = null
const listeners = new Set()
let subscribed = false

function bump(event) {
  latest = event
  if (event && event.event === 'ERROR') {
    const cmd = event.data && event.data.command
    notifyError('Wallpaper error', cmd ? `command ${cmd} failed` : 'the wallpaper engine reported an error')
  } else if (event && event.event === 'recovery-gave-up') {
    notifyError('Wallpaper recovery failed', 'the last wallpaper could not be restored')
  }
  for (const listener of listeners) listener(event)
}

function ensureSubscribed() {
  if (subscribed || !window.rexDesktop?.onWallpaperEvent) return
  subscribed = true
  window.rexDesktop.onWallpaperEvent(bump)
}

export function useWallpaperRuntimeStatus() {
  const [status, setStatus] = useState(latest)

  useEffect(() => {
    ensureSubscribed()
    const listener = (event) => setStatus(event)
    listeners.add(listener)
    if (latest) setStatus(latest)
    return () => listeners.delete(listener)
  }, [])

  return status
}
