import test from 'node:test'
import assert from 'node:assert/strict'

import {
  applySettings,
  fitModesFor,
  isLiveProperty,
  normalizeDisplayMode,
  pickLiveSettings,
  sanitizeSettings,
} from './wallpaperSettings.js'

const wallpaper = () => ({
  id: 'w1',
  type: 'VIDEO',
  filePath: 'D:/wallpapers/clip.mp4',
  display: { mode: 'Cover', monitor: 'Monitor 1', offsetX: 0 },
  playback: {
    loop: true,
    mute: false,
    volume: 100,
    speed: 1,
    audioMode: 'auto',
    autoplay: true,
    playing: true,
  },
  appearance: { brightness: 100, contrast: 100, saturation: 100, opacity: 100 },
})

test('pickLiveSettings keeps only the engine-backed properties', () => {
  const settings = pickLiveSettings(wallpaper())

  assert.deepEqual(settings, {
    display: { mode: 'Cover' },
    playback: { loop: true, mute: false, volume: 100, speed: 1, audioMode: 'auto' },
    appearance: { brightness: 100, contrast: 100, saturation: 100 },
  })
})

test('pickLiveSettings clamps and defaults bad values', () => {
  const w = wallpaper()
  w.display.mode = 'Nonsense'
  w.playback.volume = 150
  w.playback.speed = 0
  w.playback.audioMode = 'atmos'
  w.playback.loop = 'yes'

  const settings = pickLiveSettings(w)

  assert.equal(settings.display.mode, 'Cover')
  assert.equal(settings.playback.volume, 100)
  assert.equal(settings.playback.speed, 0.25)
  assert.equal(settings.playback.audioMode, 'auto')
  assert.equal(settings.playback.loop, true)
})

test('sanitizeSettings rejects non-objects', () => {
  for (const raw of [null, undefined, 42, 'nope', [], true]) {
    assert.deepEqual(sanitizeSettings(raw), {})
  }
})

test('sanitizeSettings drops unknown fields and invalid values', () => {
  const clean = sanitizeSettings({
    display: { mode: 'Stretch', offsetX: 12, bogus: true },
    playback: { volume: 40, autoplay: false, mute: 'yes', speed: 'fast', audioMode: 'stereo' },
    appearance: { opacity: 10 },
  })

  assert.deepEqual(clean, {
    display: { mode: 'Stretch' },
    playback: { volume: 40, audioMode: 'stereo' },
  })
})

test('sanitizeSettings normalizes legacy display mode names', () => {
  assert.equal(normalizeDisplayMode('Fill'), 'Cover')
  assert.equal(normalizeDisplayMode('Span'), 'Cover')
  assert.equal(normalizeDisplayMode('Fit Width'), 'Cover')
  assert.equal(normalizeDisplayMode('Fit Height'), 'Contain')
  assert.equal(normalizeDisplayMode('wat'), null)
})

test('applySettings merges a partial payload without mutating the source', () => {
  const source = wallpaper()
  const next = applySettings(source, {
    display: { mode: 'Contain' },
    playback: { volume: 35, mute: true },
  })

  assert.notEqual(next, source)
  assert.equal(next.display.mode, 'Contain')
  assert.equal(next.display.monitor, 'Monitor 1')
  assert.equal(next.playback.volume, 35)
  assert.equal(next.playback.mute, true)
  // Untouched playback fields survive hydration.
  assert.equal(next.playback.autoplay, true)
  // Source is untouched.
  assert.equal(source.display.mode, 'Cover')
  assert.equal(source.playback.volume, 100)
  assert.equal(source.playback.mute, false)
})

test('applySettings returns null when the payload has nothing usable', () => {
  assert.equal(applySettings(wallpaper(), { playback: { volume: 'loud' } }), null)
  assert.equal(applySettings(wallpaper(), null), null)
})

test('isLiveProperty recognises the engine-backed paths only', () => {
  assert.equal(isLiveProperty(['display', 'mode']), true)
  for (const key of ['loop', 'mute', 'volume', 'speed', 'audioMode']) {
    assert.equal(isLiveProperty(['playback', key]), true)
  }
  for (const key of ['brightness', 'contrast', 'saturation']) {
    assert.equal(isLiveProperty(['appearance', key]), true)
  }
  assert.equal(isLiveProperty(['display', 'monitor']), false)
  assert.equal(isLiveProperty(['appearance', 'opacity']), false)
  assert.equal(isLiveProperty(['playback', 'playing']), false)
})

test('videos cannot tile: the engine rejects the mode', () => {
  assert.equal(fitModesFor('VIDEO').includes('Tile'), false)
  // Windows tiles images natively, so the OS path keeps it.
  assert.equal(fitModesFor('IMAGE').includes('Tile'), true)
  assert.equal(fitModesFor('IMAGE').includes('Cover'), true)
})

test('the three picture fields are live, persisted and clamped to 0..200', () => {
  const w = wallpaper()
  w.appearance = { brightness: 150, contrast: 50, saturation: 100, opacity: 80, gamma: 1.4 }

  const settings = pickLiveSettings(w)

  // Only the three live fields travel; the other appearance keys stay dead.
  assert.deepEqual(settings.appearance, { brightness: 150, contrast: 50, saturation: 100 })
  assert.equal(isLiveProperty(['appearance', 'brightness']), true)
  assert.equal(isLiveProperty(['appearance', 'contrast']), true)
  assert.equal(isLiveProperty(['appearance', 'saturation']), true)
  assert.equal(isLiveProperty(['appearance', 'opacity']), false)
})

test('picture values are clamped into 0..200 and bad ones are dropped', () => {
  const clean = sanitizeSettings({
    appearance: { brightness: 250, contrast: -10, saturation: 'bright', opacity: 30 },
  })

  assert.deepEqual(clean.appearance, { brightness: 200, contrast: 0 })
})

test('applySettings restores the picture fields and leaves other appearance keys alone', () => {
  const source = wallpaper()
  source.appearance = { brightness: 100, contrast: 100, saturation: 100, opacity: 100, gamma: 1 }

  const next = applySettings(source, { appearance: { brightness: 175 } })

  assert.equal(next.appearance.brightness, 175)
  assert.equal(next.appearance.contrast, 100)
  assert.equal(next.appearance.opacity, 100)
})

test('every offered fit mode is one the engine understands', () => {
  for (const type of ['VIDEO', 'IMAGE']) {
    for (const mode of fitModesFor(type)) {
      assert.equal(normalizeDisplayMode(mode), mode)
    }
  }
})
