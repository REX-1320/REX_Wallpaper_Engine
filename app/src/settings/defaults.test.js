import test from 'node:test'
import assert from 'node:assert/strict'
import {
  DEFAULT_DEFAULTS,
  sanitizeDefaults,
  applyDefaults,
  FIT_MODES,
} from './defaults.js'
import { applySettings } from '../wallpapers/wallpaperSettings.js'

test('the defaults are the engine-neutral baseline', () => {
  assert.deepEqual(DEFAULT_DEFAULTS, {
    display: { mode: 'Cover' },
    playback: { loop: true, mute: false, volume: 100, speed: 1 },
    appearance: { brightness: 100, contrast: 100, saturation: 100 },
  })
})

test('sanitize keeps only known fields, clamps the ranges and fills the gaps', () => {
  const clean = sanitizeDefaults({
    display: { mode: 'Stretch', offsetX: 12 },
    playback: { volume: 500, speed: 0, mute: 'yes', loop: false, audioMode: 'atmos' },
    appearance: { brightness: 250, contrast: -5, saturation: 120, opacity: 40 },
  })

  assert.deepEqual(clean, {
    display: { mode: 'Stretch' },
    playback: { loop: false, mute: false, volume: 100, speed: 0.25 },
    appearance: { brightness: 200, contrast: 0, saturation: 120 },
  })
})

test('an unknown fit mode falls back to Cover', () => {
  assert.equal(sanitizeDefaults({ display: { mode: 'Nonsense' } }).display.mode, 'Cover')
  assert.ok(FIT_MODES.includes('Contain'))
})

test('applyDefaults sets the live fields - the stored settings are applied after it and win', () => {
  const wallpaper = {
    display: { mode: 'Contain' },
    playback: { volume: 35, mute: true, loop: false, speed: 1.5 },
    appearance: { brightness: 150, contrast: 100, saturation: 100 },
  }

  const withDefaults = applyDefaults(wallpaper, {
    display: { mode: 'Cover' },
    playback: { volume: 100, mute: false, loop: true, speed: 1 },
    appearance: { brightness: 100, contrast: 100, saturation: 100 },
  })

  // The defaults REPLACE the registry's placeholder values.
  assert.equal(withDefaults.display.mode, 'Cover')
  assert.equal(withDefaults.playback.volume, 100)
  assert.equal(withDefaults.playback.mute, false)
  assert.equal(withDefaults.appearance.brightness, 100)

  // The file's own saved settings are applied after that, and they win.
  const stored = applySettings(withDefaults, { playback: { volume: 35, mute: true } })
  assert.equal(stored.playback.volume, 35)
  assert.equal(stored.playback.mute, true)
  assert.equal(stored.display.mode, 'Cover')
})

test('applyDefaults fills a wallpaper that has no live values yet', () => {
  const next = applyDefaults({ display: {}, playback: {}, appearance: {} }, {
    display: { mode: 'Stretch' },
    playback: { volume: 60, mute: true, loop: true, speed: 1 },
    appearance: { brightness: 120, contrast: 100, saturation: 100 },
  })

  assert.equal(next.display.mode, 'Stretch')
  assert.equal(next.playback.volume, 60)
  assert.equal(next.playback.mute, true)
  assert.equal(next.appearance.brightness, 120)
})
