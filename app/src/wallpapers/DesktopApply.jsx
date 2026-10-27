import { useEffect, useState } from 'react'
import { adoptAppliedPath, seedWallpapers, useWallpaperStore, getSnapshot } from './store'
import { applyWallpaperToDesktop, updateDesktopSettings } from './desktop'
import { useWallpaperRuntimeStatus } from './runtimeEvents'
import { playlistEngine } from './playlist/engine'

export function reapplyDesktopWallpaper() {
  const { wallpapers, selectedId, appliedId } = getSnapshot()
  const active = wallpapers.find((w) => w.id === (appliedId ?? selectedId)) ?? wallpapers[0] ?? null
  if (active) applyWallpaperToDesktop(active)
}

export default function DesktopApply() {
  const { wallpapers, appliedId, externallyApplied } = useWallpaperStore()
  const [loadedKey, setLoadedKey] = useState(null)
  const runtimeStatus = useWallpaperRuntimeStatus()

  useEffect(() => {
    seedWallpapers()
  }, [])

  // The playlist engine follows the app, not the gallery tab: starting it
  // here means switching pages cannot silently stop a rotation, and main keeps
  // Timer/scene-end running after this window is destroyed (stopping here only
  // releases the local view; it never clears main's plan).
  useEffect(() => {
    playlistEngine.start()
    return () => playlistEngine.stop()
  }, [])

  useEffect(() => {
    if (runtimeStatus?.event === 'ERROR') {
      console.warn('[runtime]', runtimeStatus.event, runtimeStatus.data)
    } else if (runtimeStatus?.event) {
      console.info('[runtime]', runtimeStatus.event, runtimeStatus.data)
    }
  }, [runtimeStatus])

  // A verified crash recovery replayed the file main already had. Adopt it as
  // "on desktop" WITHOUT a second LOAD (externallyApplied makes DesktopApply
  // skip the apply) so the UI and the runtime agree again.
  useEffect(() => {
    if (runtimeStatus?.event !== 'recovery-ok') return
    const path = runtimeStatus.data?.path
    if (!path) return
    adoptAppliedPath(path, { keepSelection: true }).catch(() => {})
  }, [runtimeStatus])

  // Only an explicitly applied wallpaper drives the desktop; selecting a
  // tile for preview (single click) must not change the wallpaper.
  const active = wallpapers.find((w) => w.id === appliedId) ?? null

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
        active.playback.audioMode,
        active.appearance?.brightness,
        active.appearance?.contrast,
        active.appearance?.saturation,
      ].join('|')
    : null

  useEffect(() => {
    if (!active) return
    // At boot main has already replayed last-wallpaper.json; adopt that scene
    // instead of issuing a second LOAD (which would restart the video just
    // because the user selected its tile).
    if (loadKey !== null && !externallyApplied) applyWallpaperToDesktop(active)
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
