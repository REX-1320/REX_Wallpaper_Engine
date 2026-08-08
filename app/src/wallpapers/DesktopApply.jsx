import { useEffect, useState } from 'react'
import { seedWallpapers, useWallpaperStore, getSnapshot } from './store'
import { applyWallpaperToDesktop, updateDesktopSettings } from './desktop'
import { useWallpaperRuntimeStatus } from './runtimeEvents'

export function reapplyDesktopWallpaper() {
  const { wallpapers, selectedId } = getSnapshot()
  const active = wallpapers.find((w) => w.id === selectedId) ?? wallpapers[0] ?? null
  if (active) applyWallpaperToDesktop(active)
}

export default function DesktopApply() {
  const { wallpapers, selectedId } = useWallpaperStore()
  const [loadedKey, setLoadedKey] = useState(null)
  const runtimeStatus = useWallpaperRuntimeStatus()

  useEffect(() => {
    seedWallpapers()
  }, [])

  useEffect(() => {
    if (runtimeStatus?.event === 'ERROR') {
      console.warn('[runtime]', runtimeStatus.event, runtimeStatus.data)
    } else if (runtimeStatus?.event) {
      console.info('[runtime]', runtimeStatus.event, runtimeStatus.data)
    }
  }, [runtimeStatus])

  const active = wallpapers.find((w) => w.id === selectedId) ?? wallpapers[0] ?? null

  // Scene identity — a new file/type means a fresh LOAD+PLAY on the runtime.
  const loadKey = active ? [active.id, active.type, active.filePath].join('|') : null

  // Live-adjustable properties — sent as SET_* commands without a reload.
  const settingsKey = active
    ? [
        active.display.mode,
        active.playback.loop,
        active.playback.mute,
        active.playback.volume,
        active.playback.speed,
      ].join('|')
    : null

  useEffect(() => {
    if (!active) return
    if (loadKey !== null) applyWallpaperToDesktop(active)
    // Track the applied scene so property updates only flow once the
    // runtime has a scene loaded (otherwise SET_* would race the LOAD).
    setLoadedKey(loadKey)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loadKey])

  useEffect(() => {
    if (!active) return
    if (loadedKey === null || loadedKey !== loadKey) return
    updateDesktopSettings(active)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [settingsKey])

  return null
}
