import { useEffect, useState } from 'react'

// ─── Runtime event bridge ──────────────────────────────────────────────────
// Subscribes once to the Electron bridge (window.rexDesktop.onWallpaperEvent)
// and fans events out to React hooks. Events: READY, LOADED, PLAYING,
// PAUSED, STOPPED, UNLOADED, ERROR — each `{ event, data }`.

let latest = null
const listeners = new Set()
let subscribed = false

function bump(event) {
  latest = event
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
