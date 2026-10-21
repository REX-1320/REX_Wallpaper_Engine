// Per-wallpaper live settings: the only fields the runtime can change without
// reloading the scene (see DesktopApply settingsKey). Kept dependency-free so
// the persistence/hydration rules are node-testable.
//
// Pure: no React, no Electron bridge, no store access.

export const DISPLAY_MODES = ['Cover', 'Contain', 'Stretch', 'Center', 'Tile']

// The engine implements Fill, Fit, Stretch and Center. Tile is an OS-only
// mode — Windows tiles images natively — while the engine scene path rejects
// it outright ("Tile fill mode is not implemented"), so it must never be
// offered for a video.
export const FIT_MODES_BY_TYPE = {
  IMAGE: ['Cover', 'Contain', 'Stretch', 'Center', 'Tile'],
  VIDEO: ['Cover', 'Contain', 'Stretch', 'Center'],
}

export function fitModesFor(type) {
  return FIT_MODES_BY_TYPE[type] ?? FIT_MODES_BY_TYPE.VIDEO
}

export const AUDIO_MODES = ['auto', 'mono', 'stereo', 'quad', 'surround51']

export const PLAYBACK_DEFAULTS = {
  loop: true,
  mute: false,
  volume: 100,
  speed: 1,
  audioMode: 'auto',
}

// Picture adjustments. The engine applies brightness / contrast / saturation
// live (SET_BRIGHTNESS & co. -> the renderer's b0 constants, re-packed every
// frame) and clamps them to 0..2 with 1.0 = unchanged. The app keeps the
// friendlier percent scale 0..200 where 100 = unchanged, and converts at the
// IPC boundary (desktop.js), exactly as `volume` does.
//
// These three are the ONLY appearance keys the engine can apply: the static
// (image) adapter has no colour pipeline, so the group is video-only.
export const PICTURE_KEYS = ['brightness', 'contrast', 'saturation']

export const PICTURE_DEFAULTS = {
  brightness: 100,
  contrast: 100,
  saturation: 100,
}

// Legacy/OS names that map onto the engine's five modes. The old inspector
// offered nine; the engine collapses Span/Fit Width to fill and Fit Height to
// fit (see ENGINE_MODE_BY_MODE in desktop.js).
const LEGACY_DISPLAY_MODES = {
  Fill: 'Cover',
  Span: 'Cover',
  'Fit Width': 'Cover',
  'Fit Height': 'Contain',
}

const LIVE_PATHS = new Set([
  'display.mode',
  'playback.loop',
  'playback.mute',
  'playback.volume',
  'playback.speed',
  'playback.audioMode',
  'appearance.brightness',
  'appearance.contrast',
  'appearance.saturation',
])

export function normalizeDisplayMode(mode) {
  if (DISPLAY_MODES.includes(mode)) return mode
  return LEGACY_DISPLAY_MODES[mode] ?? null
}

function clampNumber(value, min, max) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null
  return Math.min(max, Math.max(min, value))
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
}

// Accepts whatever came out of disk (possibly hand-edited or from an older
// build) and returns only known-good live fields. Invalid values are dropped,
// not defaulted, so a corrupt entry cannot silently reset a working scene.
export function sanitizeSettings(raw) {
  if (!isPlainObject(raw)) return {}

  const result = {}

  if (isPlainObject(raw.display)) {
    const mode = typeof raw.display.mode === 'string' ? normalizeDisplayMode(raw.display.mode) : null
    if (mode) result.display = { mode }
  }

  if (isPlainObject(raw.playback)) {
    const playback = {}
    if (typeof raw.playback.loop === 'boolean') playback.loop = raw.playback.loop
    if (typeof raw.playback.mute === 'boolean') playback.mute = raw.playback.mute
    const volume = clampNumber(raw.playback.volume, 0, 100)
    if (volume !== null) playback.volume = volume
    const speed = clampNumber(raw.playback.speed, 0.25, 4)
    if (speed !== null) playback.speed = speed
    if (AUDIO_MODES.includes(raw.playback.audioMode)) playback.audioMode = raw.playback.audioMode
    if (Object.keys(playback).length > 0) result.playback = playback
  }

  if (isPlainObject(raw.appearance)) {
    const appearance = {}
    for (const key of PICTURE_KEYS) {
      const value = clampNumber(raw.appearance[key], 0, 200)
      if (value !== null) appearance[key] = value
    }
    if (Object.keys(appearance).length > 0) result.appearance = appearance
  }

  return result
}

// Serialisable snapshot written into the gallery library entry.
export function pickLiveSettings(wallpaper) {
  const clean = sanitizeSettings({
    display: { mode: wallpaper?.display?.mode },
    playback: {
      loop: wallpaper?.playback?.loop,
      mute: wallpaper?.playback?.mute,
      volume: wallpaper?.playback?.volume,
      speed: wallpaper?.playback?.speed,
      audioMode: wallpaper?.playback?.audioMode,
    },
    appearance: {
      brightness: wallpaper?.appearance?.brightness,
      contrast: wallpaper?.appearance?.contrast,
      saturation: wallpaper?.appearance?.saturation,
    },
  })

  return {
    display: { mode: clean.display?.mode ?? 'Cover' },
    playback: { ...PLAYBACK_DEFAULTS, ...(clean.playback ?? {}) },
    appearance: { ...PICTURE_DEFAULTS, ...(clean.appearance ?? {}) },
  }
}

// Returns a new wallpaper with persisted settings merged over its defaults.
// Null means "nothing usable in the payload" so callers can skip a re-render.
export function applySettings(wallpaper, settings) {
  if (!wallpaper) return null
  const clean = sanitizeSettings(settings)
  if (!clean.display && !clean.playback && !clean.appearance) return null

  const next = { ...wallpaper }
  if (clean.display) next.display = { ...(wallpaper.display ?? {}), ...clean.display }
  if (clean.playback) next.playback = { ...(wallpaper.playback ?? {}), ...clean.playback }
  if (clean.appearance) next.appearance = { ...(wallpaper.appearance ?? {}), ...clean.appearance }
  return next
}

// True for the six properties DesktopApply pushes live; only these are worth
// writing to disk (everything else is either static or a dead legacy field).
export function isLiveProperty(path) {
  if (!Array.isArray(path) || path.length === 0) return false
  return LIVE_PATHS.has(path.join('.'))
}
