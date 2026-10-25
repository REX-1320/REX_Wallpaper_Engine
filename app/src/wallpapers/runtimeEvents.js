import { useEffect, useState } from 'react'

import { notifyError } from './notify'

// ─── Runtime event bridge ──────────────────────────────────────────────────
// Subscribes once to the Electron bridge (window.paiintDesktop.onWallpaperEvent)
// and fans events out to React hooks. Events: READY, LOADED, PLAYING,
// PAUSED, STOPPED, UNLOADED, ERROR — each `{ event, data }`, plus supervisor
// recovery events (recovery-start/ok/gave-up/parked-poison). ERROR,
// recovery-gave-up and recovery-parked-poison surface as user-facing toasts
// (P1 error-to-toast path); the last valid wallpaper stays on screen
// (failed LOADs never clear it).

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
  } else if (event && event.event === 'recovery-parked-poison') {
    const p = event.data && event.data.path
    const name = typeof p === 'string' ? p.split(/[\\/]/).pop() : null
    notifyError(
      'Wallpaper parked',
      name
        ? `"${name}" keeps crashing the wallpaper engine, so it was parked`
        : 'a file keeps crashing the wallpaper engine, so it was parked',
    )
  }
  for (const listener of listeners) listener(event)
}

function ensureSubscribed() {
  if (subscribed || !window.paiintDesktop?.onWallpaperEvent) return
  subscribed = true
  window.paiintDesktop.onWallpaperEvent(bump)
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
