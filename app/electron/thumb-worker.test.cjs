// Poster selection policy for the GPU thumbnail worker.
//
// The policy is pure so it can be tested without Electron: which timestamps to
// sample for a poster, and whether a decoded frame is effectively blank (a
// black frame or a white title card is not a useful gallery poster).
const test = require('node:test')
const assert = require('node:assert/strict')

const { posterSeekTimes, lumaStats, isFlatFrame } = require('./thumb-worker.cjs')

test('posterSeekTimes samples 20% into a short wallpaper loop first', () => {
  assert.deepEqual(posterSeekTimes(10), [2, 5, 1, 7, 0.5])
})

test('posterSeekTimes caps the first sample for a feature-length file', () => {
  const times = posterSeekTimes(8701)
  assert.equal(times[0], 600)
  assert.deepEqual(times, [600, 4350.5, 870.1, 6090.7, 0.5])
})

test('posterSeekTimes keeps candidates strictly inside the clip', () => {
  assert.deepEqual(posterSeekTimes(1), [0.5, 0.1, 0.7])
})

test('posterSeekTimes has a usable fallback when duration is unknown', () => {
  assert.deepEqual(posterSeekTimes(0), [0.5])
  assert.deepEqual(posterSeekTimes(NaN), [0.5])
})

test('lumaStats reports mean and deviation of an RGBA frame', () => {
  const black = new Uint8ClampedArray([0, 0, 0, 255, 0, 0, 0, 255])
  assert.deepEqual(lumaStats(black), { mean: 0, std: 0 })

  const blackAndWhite = new Uint8ClampedArray([0, 0, 0, 255, 255, 255, 255, 255])
  const stats = lumaStats(blackAndWhite)
  assert.equal(Math.round(stats.mean * 10) / 10, 127.5)
  assert.equal(Math.round(stats.std * 10) / 10, 127.5)
})

test('isFlatFrame rejects black frames, white title cards and near-uniform frames', () => {
  assert.equal(isFlatFrame({ mean: 0, std: 0 }), true)
  assert.equal(isFlatFrame({ mean: 254.5, std: 1 }), true)
  assert.equal(isFlatFrame({ mean: 18, std: 2.9 }), true)
  assert.equal(isFlatFrame({ mean: 128, std: 12 }), false)
})
