const test = require('node:test')
const assert = require('node:assert/strict')
const { resolveWindowBounds } = require('./window-bounds.cjs')

const display = (x, y, width, height) => ({ bounds: { x, y, width, height } })

test('nothing saved means nothing to restore', () => {
  assert.equal(resolveWindowBounds(null, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 }), null)
  assert.equal(resolveWindowBounds(undefined, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 }), null)
})

test('a saved window on a live display is restored as-is', () => {
  const saved = { x: 100, y: 120, width: 1000, height: 700 }
  const out = resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 })
  assert.deepEqual(out, saved)
})

test('a saved window on a disconnected monitor falls back to null', () => {
  const saved = { x: 4000, y: 200, width: 1000, height: 700 }
  assert.equal(resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 }), null)
})

test('a saved window that only partly overlaps a display is still restored', () => {
  const saved = { x: 1800, y: 1000, width: 400, height: 300 }
  const out = resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 })
  assert.deepEqual(out, saved)
})

test('a saved size below the minimum is not restored', () => {
  const saved = { x: 10, y: 10, width: 10, height: 10 }
  const fallback = { width: 1280, height: 800, minWidth: 960, minHeight: 640 }
  assert.equal(resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], fallback), null)
})

test('a saved window on the second display of a multi-monitor setup is restored', () => {
  const saved = { x: 2200, y: 300, width: 1200, height: 800 }
  const out = resolveWindowBounds(saved, [display(0, 0, 1920, 1080), display(1920, 0, 2560, 1440)], { width: 1280, height: 800 })
  assert.deepEqual(out, saved)
})
