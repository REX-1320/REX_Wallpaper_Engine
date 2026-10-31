const test = require('node:test')
const assert = require('node:assert/strict')
const { isWindowOpen, effectiveMute, desktopCovered } = require('./audio-visibility.cjs')

const fakeWindow = ({ destroyed = false, visible = true, minimized = false } = {}) => ({
  isDestroyed: () => destroyed,
  isVisible: () => visible,
  isMinimized: () => minimized,
})

test('no window is not open', () => {
  assert.equal(isWindowOpen(null), false)
  assert.equal(isWindowOpen(undefined), false)
})

test('a destroyed window is not open', () => {
  assert.equal(isWindowOpen(fakeWindow({ destroyed: true })), false)
})

test('a hidden window is not open', () => {
  assert.equal(isWindowOpen(fakeWindow({ visible: false })), false)
})

test('a minimized window is not open', () => {
  assert.equal(isWindowOpen(fakeWindow({ minimized: true })), false)
})

test('a visible, restored window is open', () => {
  assert.equal(isWindowOpen(fakeWindow()), true)
})

test('the override only adds mute', () => {
  assert.equal(effectiveMute({ intentMuted: false, onlyWhenClosed: false, windowOpen: false }), false)
  assert.equal(effectiveMute({ intentMuted: false, onlyWhenClosed: false, windowOpen: true }), false)
  assert.equal(effectiveMute({ intentMuted: false, onlyWhenClosed: true, windowOpen: false }), false)
  assert.equal(effectiveMute({ intentMuted: false, onlyWhenClosed: true, windowOpen: true }), true)
})

test('a user-muted wallpaper stays muted in every state', () => {
  for (const onlyWhenClosed of [false, true]) {
    for (const windowOpen of [false, true]) {
      assert.equal(effectiveMute({ intentMuted: true, onlyWhenClosed, windowOpen }), true)
    }
  }
})

test('missing values are treated as off', () => {
  assert.equal(effectiveMute({}), false)
  assert.equal(effectiveMute({ intentMuted: false, onlyWhenClosed: true }), false)
})

test('before the monitor reports, the Paiint window decides', () => {
  assert.equal(desktopCovered({ occlusionReported: false, desktopOccluded: false, windowOpen: true }), true)
  assert.equal(desktopCovered({ occlusionReported: false, desktopOccluded: true, windowOpen: false }), false)
})

test('after the monitor reports, its enumeration is authoritative', () => {
  // Win+D: the Paiint window is still "open" but the desktop is on top.
  assert.equal(desktopCovered({ occlusionReported: true, desktopOccluded: false, windowOpen: true }), false)
  // Paiint itself covers the desktop; the monitor counted it.
  assert.equal(desktopCovered({ occlusionReported: true, desktopOccluded: true, windowOpen: true }), true)
  assert.equal(desktopCovered({ occlusionReported: true, desktopOccluded: true, windowOpen: false }), true)
})

test('a missing monitor report falls back to the window', () => {
  assert.equal(desktopCovered({}), false)
  assert.equal(desktopCovered({ windowOpen: true }), true)
})
