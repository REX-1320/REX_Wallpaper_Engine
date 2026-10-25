import { notifyError } from './notify'
import { effectiveCapabilities } from './capabilities'

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
//
// The video adapter also advertises brightness / contrast / saturation (it
// applies them per frame through the renderer's b0 constants). The static
// (image) adapter has NO colour pipeline, so those are video-only.
export const DESKTOP_CAPABILITIES = {
  IMAGE: ['fillMode'],
  VIDEO: ['fillMode', 'speed', 'loop', 'volume', 'mute', 'audioMode', 'brightness', 'contrast', 'saturation'],
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
  if (w.filePath.toLowerCase().startsWith('c:\\paiint\\')) return
  if (w.type === 'IMAGE') {
    const style = DESKTOP_STYLE_BY_MODE[w.display.mode] ?? 'cover'
    window.paiintDesktop?.setDesktopVideo?.({ path: null })
    window.paiintDesktop?.setDesktopWallpaper?.(w.filePath, style)?.then((res) => {
      if (res && !res.ok) notifyError('Could not apply image wallpaper', res.error)
    })
    return
  }
  if (w.type === 'VIDEO') {
    const mode = ENGINE_MODE_BY_MODE[w.display.mode] ?? 'fill'
    window.paiintDesktop?.setDesktopVideo?.({
      path: w.filePath,
      mode,
      fps: 0,
      loop: w.playback.loop,
      muted: w.playback.mute,
      volume: w.playback.volume / 100,
      speed: w.playback.speed,
      // The engine's colour scale is 0..2 with 1.0 = unchanged; the app
      // keeps percent 0..200 where 100 = unchanged.
      brightness: (w.appearance?.brightness ?? 100) / 100,
      contrast: (w.appearance?.contrast ?? 100) / 100,
      saturation: (w.appearance?.saturation ?? 100) / 100,
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
  if (!w || !w.filePath || w.filePath.toLowerCase().startsWith('c:\\paiint\\')) return
  if (!window.paiintDesktop?.updateWallpaper) return

  const caps = await effectiveCapabilities(DESKTOP_CAPABILITIES[w.type] ?? [], window.paiintDesktop)
  const opts = {}
  if (w.type === 'IMAGE') {
    // SET_FILL_MODE speaks the ENGINE vocabulary (fill/fit/stretch/center/
    // tile); DESKTOP_STYLE_BY_MODE's cover/stretched belong to the OS
    // wallpaper path (wallpaper:set) and were rejected here (audit Q2).
    if (caps.includes('fillMode')) opts.mode = ENGINE_MODE_BY_MODE[w.display.mode] ?? 'fill'
  } else if (w.type === 'VIDEO') {
    if (caps.includes('fillMode')) opts.mode = ENGINE_MODE_BY_MODE[w.display.mode] ?? 'fill'
    if (caps.includes('loop')) opts.loop = w.playback.loop
    if (caps.includes('speed')) opts.speed = w.playback.speed
    if (caps.includes('volume')) opts.volume = w.playback.volume / 100
    if (caps.includes('mute')) opts.muted = w.playback.mute
    if (caps.includes('audioMode') && w.playback.audioMode) {
      opts.audioMode = w.playback.audioMode
    }
    // Colour adjustments: the engine clamps 0..2 with 1.0 = unchanged, the
    // app stores percent 0..200 with 100 = unchanged.
    if (caps.includes('brightness')) opts.brightness = (w.appearance?.brightness ?? 100) / 100
    if (caps.includes('contrast')) opts.contrast = (w.appearance?.contrast ?? 100) / 100
    if (caps.includes('saturation')) opts.saturation = (w.appearance?.saturation ?? 100) / 100
  } else {
    return
  }

  try {
    const res = await window.paiintDesktop.updateWallpaper(opts)
    if (res && !res.ok) notifyError('Could not update wallpaper', res.error)
    else if (res?.skipped?.length) console.info('updateWallpaper skipped (unsupported):', res.skipped)
  } catch (err) {
    notifyError('Could not update wallpaper', err.message)
  }
}
