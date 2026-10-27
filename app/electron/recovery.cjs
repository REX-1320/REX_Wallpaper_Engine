'use strict'

// Recovery helpers shared by the boot replay and the supervisor's crash
// recovery.
//
// last-wallpaper.json is written on apply and after live SET_* commands, while
// the gallery library stores the per-file settings the inspector edits. The
// two writes are debounced independently, so after a crash either can be the
// newer one. The library is the durable per-file record: merge it over the
// persisted entry before replaying, so recovery restores what the user last
// saw rather than the apply-time snapshot.
//
// Pure, so the conversions are unit-testable: see recovery.test.cjs.

const FIT_MODE_TO_ENGINE = {
  Cover: 'fill',
  Contain: 'fit',
  Stretch: 'stretch',
  Center: 'center',
  Tile: 'tile',
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value))
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value)
}

// The library stores UI units (0..200 %, 0..100 volume); the runtime takes
// engine units (0..2 scale, 0..1 volume).
function mergeRecoveryEntry(entry, settings) {
  if (!entry || !entry.params || !settings || typeof settings !== 'object') return entry

  const params = { ...entry.params }
  const display = settings.display
  if (display && typeof display.mode === 'string' && FIT_MODE_TO_ENGINE[display.mode]) {
    params.mode = FIT_MODE_TO_ENGINE[display.mode]
  }

  const playback = settings.playback
  if (playback && typeof playback === 'object') {
    if (typeof playback.loop === 'boolean') params.loop = playback.loop
    if (typeof playback.mute === 'boolean') params.muted = playback.mute
    if (finite(playback.volume)) params.volume = clamp(playback.volume, 0, 100) / 100
    if (finite(playback.speed)) params.speed = clamp(playback.speed, 0.25, 4)
    if (typeof playback.audioMode === 'string' && playback.audioMode) params.audioMode = playback.audioMode
  }

  const appearance = settings.appearance
  if (appearance && typeof appearance === 'object') {
    for (const key of ['brightness', 'contrast', 'saturation']) {
      if (finite(appearance[key])) params[key] = clamp(appearance[key], 0, 200) / 100
    }
  }

  return { ...entry, params }
}

// Is the native scene actually on the desktop?
//
// EngineState: 3 = Running, 4 = Paused. A paused scene still owns the WorkerW
// layer (fullscreen auto-pause is expected), so both count as recovered; a
// scene with zero duration has not decoded anything yet. Any other state is a
// failed replay.
function desktopLayerReady(status) {
  if (!status || typeof status !== 'object') return false
  const state = Number(status.state)
  if (state === 3) return status.playing === true
  if (state === 4) return Number(status.duration) > 0
  return false
}

module.exports = { FIT_MODE_TO_ENGINE, desktopLayerReady, mergeRecoveryEntry }
