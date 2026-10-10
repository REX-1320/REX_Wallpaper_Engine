import { notifyError } from './notify'

export const DESKTOP_STYLE_BY_MODE = {
  Cover: 'cover',
  Fill: 'fill',
  Span: 'cover',
  'Fit Width': 'cover',
  Tile: 'tile',
  Contain: 'fit',
  'Fit Height': 'fit',
  Stretch: 'stretched',
  Center: 'center',
}

export const ENGINE_MODE_BY_MODE = {
  Cover: 'fill',
  Fill: 'fill',
  Span: 'fill',
  'Fit Width': 'fill',
  Tile: 'tile',
  Contain: 'fit',
  'Fit Height': 'fit',
  Stretch: 'stretch',
  Center: 'center',
}

// What the C++ engine can apply live, per type. Mirrors the runtime's
// GET_CAPABILITIES response (used as a fallback when the runtime is
// unavailable, e.g. browser-only dev).
export const DESKTOP_CAPABILITIES = {
  IMAGE: ['fillMode'],
  VIDEO: ['fillMode', 'speed', 'loop', 'volume', 'mute'],
  WEB: [],
  SCENE: [],
  SHADER: [],
  AUDIO_VISUALIZER: [],
}

export function applyWallpaperToDesktop(w) {
  if (!w) return
  if (!w.filePath) {
    // File-less entries (e.g. the seeded gallery placeholder) are not
    // real wallpapers: sending STOP for them wiped the persisted
    // last-wallpaper on every cold boot (the gallery mounts with the
    // seed selected before any file exists). Do nothing instead.
    return
  }
  if (w.filePath.toLowerCase().startsWith('c:\\rex\\')) return
  if (w.type === 'IMAGE') {
    const style = DESKTOP_STYLE_BY_MODE[w.display.mode] ?? 'cover'
    window.rexDesktop?.setDesktopVideo?.({ path: null })
    window.rexDesktop?.setDesktopWallpaper?.(w.filePath, style)?.then((res) => {
      if (res && !res.ok) notifyError('Could not apply image wallpaper', res.error)
    })
    return
  }
  if (w.type === 'VIDEO') {
    const mode = ENGINE_MODE_BY_MODE[w.display.mode] ?? 'fill'
    window.rexDesktop?.setDesktopVideo?.({
      path: w.filePath,
      mode,
      fps: 0,
      loop: w.playback.loop,
      muted: w.playback.mute,
      volume: w.playback.volume / 100,
      speed: w.playback.speed,
    })?.then((res) => {
      if (res && !res.ok) notifyError('Could not apply video wallpaper', res.error)
    })
    return
  }
  // Deferred types (WEB/SCENE/SHADER/AUDIO_VISUALIZER): never clear the
  // running wallpaper for something we cannot show. Warn and keep it.
  console.warn(`applyWallpaperToDesktop: ${w.type} wallpapers are not supported yet; keeping current wallpaper.`)
}

// Live property updates after a wallpaper is already applied. Only the
// properties the runtime renderer supports are transmitted; everything
// else is dropped (and reported) instead of silently no-oping.
export async function updateDesktopSettings(w) {
  if (!w || !w.filePath || w.filePath.toLowerCase().startsWith('c:\\rex\\')) return
  if (!window.rexDesktop?.updateWallpaper) return

  const caps = DESKTOP_CAPABILITIES[w.type] ?? []
  const opts = {}
  if (w.type === 'IMAGE') {
    if (caps.includes('fillMode')) opts.mode = DESKTOP_STYLE_BY_MODE[w.display.mode] ?? 'cover'
  } else if (w.type === 'VIDEO') {
    if (caps.includes('fillMode')) opts.mode = ENGINE_MODE_BY_MODE[w.display.mode] ?? 'fill'
    if (caps.includes('loop')) opts.loop = w.playback.loop
    if (caps.includes('speed')) opts.speed = w.playback.speed
    if (caps.includes('volume')) opts.volume = w.playback.volume / 100
    if (caps.includes('mute')) opts.muted = w.playback.mute
  } else {
    return
  }

  try {
    const res = await window.rexDesktop.updateWallpaper(opts)
    if (res && !res.ok) notifyError('Could not update wallpaper', res.error)
    else if (res?.skipped?.length) console.info('updateWallpaper skipped (unsupported):', res.skipped)
  } catch (err) {
    notifyError('Could not update wallpaper', err.message)
  }
}
