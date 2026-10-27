'use strict'

const test = require('node:test')
const assert = require('node:assert/strict')

const { desktopLayerReady, mergeRecoveryEntry } = require('./recovery.cjs')

const VIDEO_ENTRY = {
  kind: 'video',
  params: {
    path: 'C:\\Wallpapers\\rain.mp4',
    mode: 'fill',
    loop: true,
    muted: false,
    volume: 1,
    speed: 1,
  },
}

test('library settings replace the stale apply-time playback values', () => {
  const merged = mergeRecoveryEntry(VIDEO_ENTRY, {
    display: { mode: 'Contain' },
    playback: { loop: false, mute: true, volume: 65, speed: 1.5, audioMode: 'stereo' },
    appearance: { brightness: 150, contrast: 80, saturation: 120 },
  })

  assert.equal(merged.params.mode, 'fit')
  assert.equal(merged.params.loop, false)
  assert.equal(merged.params.muted, true)
  assert.equal(merged.params.volume, 0.65)
  assert.equal(merged.params.speed, 1.5)
  assert.equal(merged.params.audioMode, 'stereo')
  assert.equal(merged.params.brightness, 1.5)
  assert.equal(merged.params.contrast, 0.8)
  assert.equal(merged.params.saturation, 1.2)
})

test('missing settings leave the entry untouched and never mutate it', () => {
  const before = JSON.stringify(VIDEO_ENTRY)
  assert.equal(mergeRecoveryEntry(VIDEO_ENTRY, null), VIDEO_ENTRY)
  assert.equal(JSON.stringify(VIDEO_ENTRY), before)
})

test('unknown display modes keep the persisted engine mode', () => {
  const merged = mergeRecoveryEntry(VIDEO_ENTRY, { display: { mode: 'Bogus' } })
  assert.equal(merged.params.mode, 'fill')
})

test('out-of-range values are clamped, not passed through', () => {
  const merged = mergeRecoveryEntry(VIDEO_ENTRY, {
    playback: { volume: 900, speed: 99 },
    appearance: { brightness: -10 },
  })
  assert.equal(merged.params.volume, 1)
  assert.equal(merged.params.speed, 4)
  assert.equal(merged.params.brightness, 0)
})

test('desktop layer verification accepts a running or paused scene', () => {
  assert.equal(desktopLayerReady({ state: 3, playing: true }), true)
  assert.equal(desktopLayerReady({ state: 4, playing: false, duration: 12 }), true)
})

test('desktop layer verification rejects a missing or failed scene', () => {
  assert.equal(desktopLayerReady(null), false)
  assert.equal(desktopLayerReady({ state: 6, playing: false }), false)
  assert.equal(desktopLayerReady({ state: 2, playing: false }), false)
  assert.equal(desktopLayerReady({ state: 3, playing: false, duration: 0 }), false)
})
