import test from 'node:test'
import assert from 'node:assert/strict'
import { formatStatus } from './statusFormat.js'

test('an empty payload yields no rows and an unknown state', () => {
  const out = formatStatus(null)
  assert.deepEqual(out.rows, [])
  assert.equal(out.state, 'unknown')
})

test('the live scene and the measured counters become rows', () => {
  const out = formatStatus({
    state: 3,
    playing: true,
    renderer: 'video',
    privateBytes: 314572800,
    position: 12.5,
    duration: 60,
    scene: { path: 'D:/a.mp4', fillMode: 'fill', muted: false, volume: 0.8, speed: 1 },
    telemetry: {
      renderFps: 59.8, decoderFps: 24, schedulerDrops: 3, avSyncDriftMs: 1.2,
      gpuDedicated: 268435456, gpuShared: 134217728, gpuBudget: 1073741824,
      hdrActive: false, displayMaxNits: 0, sourceFps: 23.976, displayHz: 60,
    },
  })

  const labels = out.rows.map((r) => r.label)
  assert.ok(labels.includes('Frame rate'))
  assert.ok(labels.includes('VRAM (dedicated)'))
  assert.ok(labels.includes('Dropped frames'))
  assert.ok(labels.includes('A/V drift'))
  assert.ok(labels.includes('HDR'))
  assert.equal(out.state, 'running')
})

test('a missing telemetry block does not throw and names the engine state', () => {
  // EngineState: 0 Uninitialized, 1 Initializing, 2 Initialized, 3 Running,
  // 4 Paused, 5 Stopped, 6 Error, 7 ShuttingDown.
  assert.equal(formatStatus({ state: 0, playing: false }).state, 'uninitialized')
  assert.equal(formatStatus({ state: 3, playing: true }).state, 'running')
  assert.equal(formatStatus({ state: 4, playing: false }).state, 'paused')
  assert.equal(formatStatus({ state: 5, playing: false }).state, 'stopped')
  assert.equal(formatStatus({ state: 6, playing: false }).state, 'error')
  assert.equal(formatStatus({ state: 99, playing: false }).state, 'unknown')
  assert.equal(formatStatus({ state: 0, playing: false }).rows.length, 0)
})

test('byte values are MiB and rates are rounded to one decimal', () => {
  const out = formatStatus({
    playing: true,
    // The runtime reports privateBytes in MEGABYTES (GetProcessPrivateMemoryMB),
    // while the GPU fields are raw bytes.
    privateBytes: 300,
    telemetry: { renderFps: 59.76, avSyncDriftMs: 1.234, gpuDedicated: 268435456 },
  })

  const byLabel = Object.fromEntries(out.rows.map((r) => [r.label, r.value]))
  assert.equal(byLabel['Process memory'], '300 MiB')
  assert.equal(byLabel['VRAM (dedicated)'], '256 MiB')
  assert.equal(byLabel['Frame rate'], '59.8 fps')
  assert.equal(byLabel['A/V drift'], '1.2 ms')
})

test('the file row shows the name, not the whole path', () => {
  const out = formatStatus({ playing: true, scene: { path: 'D:\\live wallpaper\\clip 120fps.mp4' } })
  const byLabel = Object.fromEntries(out.rows.map((r) => [r.label, r.value]))
  assert.equal(byLabel.File, 'clip 120fps.mp4')
})

test('an HDR display is reported as active', () => {
  const out = formatStatus({ playing: true, telemetry: { hdrActive: true, displayMaxNits: 1000 } })
  const byLabel = Object.fromEntries(out.rows.map((r) => [r.label, r.value]))
  assert.equal(byLabel.HDR, 'Active')
  assert.equal(byLabel['Display peak'], '1000 nits')
})
