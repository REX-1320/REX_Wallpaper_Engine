// The defaults applied to a wallpaper when it is first imported.
//
// They are a BASELINE, not an override: whatever the file already carries
// (the per-file settings the user saved) wins. A wallpaper that was already
// in the library keeps its own values; only a fresh import starts from here.
//
// Pure, so the rules are unit-testable (`node --test app/src/settings/defaults.test.js`).

import { normalizeDisplayMode } from '../wallpapers/wallpaperSettings.js'

export const FIT_MODES = ['Cover', 'Contain', 'Stretch', 'Center', 'Tile']

export const DEFAULT_DEFAULTS = {
  display: { mode: 'Cover' },
  playback: { loop: true, mute: false, volume: 100, speed: 1 },
  appearance: { brightness: 100, contrast: 100, saturation: 100 },
}

function clampNumber(value, min, max) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null
  return Math.min(max, Math.max(min, value))
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
}

/**
 * Accepts whatever came out of disk and returns a COMPLETE defaults object:
 * known fields only, clamped, and every field present (the missing ones
 * filled from DEFAULT_DEFAULTS). A corrupt entry must not become a broken
 * wallpaper, and the Settings page renders a complete form.
 */
export function sanitizeDefaults(raw) {
  const source = isPlainObject(raw) ? raw : {}
  const srcDisplay = isPlainObject(source.display) ? source.display : {}
  const srcPlayback = isPlainObject(source.playback) ? source.playback : {}
  const srcAppearance = isPlainObject(source.appearance) ? source.appearance : {}

  const mode = normalizeDisplayMode(srcDisplay.mode)
  const volume = clampNumber(srcPlayback.volume, 0, 100)
  const speed = clampNumber(srcPlayback.speed, 0.25, 4)
  const brightness = clampNumber(srcAppearance.brightness, 0, 200)
  const contrast = clampNumber(srcAppearance.contrast, 0, 200)
  const saturation = clampNumber(srcAppearance.saturation, 0, 200)

  return {
    display: { mode: mode && FIT_MODES.includes(mode) ? mode : DEFAULT_DEFAULTS.display.mode },
    playback: {
      loop: typeof srcPlayback.loop === 'boolean' ? srcPlayback.loop : DEFAULT_DEFAULTS.playback.loop,
      mute: typeof srcPlayback.mute === 'boolean' ? srcPlayback.mute : DEFAULT_DEFAULTS.playback.mute,
      volume: volume ?? DEFAULT_DEFAULTS.playback.volume,
      speed: speed ?? DEFAULT_DEFAULTS.playback.speed,
    },
    appearance: {
      brightness: brightness ?? DEFAULT_DEFAULTS.appearance.brightness,
      contrast: contrast ?? DEFAULT_DEFAULTS.appearance.contrast,
      saturation: saturation ?? DEFAULT_DEFAULTS.appearance.saturation,
    },
  }
}

/**
 * Applies the defaults to a wallpaper's live fields. They REPLACE the
 * registry's placeholder values ("Cover", 100 %, unmuted) - the caller
 * applies the file's own saved settings afterwards, and those win.
 */
export function applyDefaults(wallpaper, defaults) {
  if (!wallpaper) return wallpaper
  const clean = sanitizeDefaults(defaults)

  return {
    ...wallpaper,
    display: {
      ...(wallpaper.display ?? {}),
      mode: clean.display.mode,
    },
    playback: {
      ...(wallpaper.playback ?? {}),
      loop: clean.playback.loop,
      mute: clean.playback.mute,
      volume: clean.playback.volume,
      speed: clean.playback.speed,
    },
    appearance: {
      ...(wallpaper.appearance ?? {}),
      brightness: clean.appearance.brightness,
      contrast: clean.appearance.contrast,
      saturation: clean.appearance.saturation,
    },
  }
}
