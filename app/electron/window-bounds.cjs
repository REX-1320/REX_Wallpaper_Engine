'use strict'

// ─── Window bounds resolution ───────────────────────────────────────────────
//
// The app remembers where the user put the window. On the next launch it must
// only restore those bounds if they are still ON a display -- a monitor that
// was unplugged since must not leave the window opening off-screen, where it
// looks like the app failed to start.
//
// Pure (no Electron import) so the rule is unit-testable: `node --test
// app/electron/window-bounds.test.cjs`.

// Does the rectangle touch the display at all? A window that only overlaps
// by a sliver is not usably visible, so a minimum overlap is required.
const MIN_VISIBLE = 64

function intersects(a, b) {
  const x = Math.max(0, Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x))
  const y = Math.max(0, Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y))
  return x >= MIN_VISIBLE && y >= MIN_VISIBLE
}

/**
 * @param {{x:number,y:number,width:number,height:number}|null} saved
 * @param {Array<{bounds:{x:number,y:number,width:number,height:number}}>} displays
 * @param {{width:number,height:number,minWidth?:number,minHeight?:number}} fallback
 * @returns {{x:number,y:number,width:number,height:number}|null} bounds to open with, or null
 */
function resolveWindowBounds(saved, displays, fallback = {}) {
  if (!saved || typeof saved !== 'object') return null
  const { x, y, width, height } = saved
  if (![x, y, width, height].every((n) => typeof n === 'number' && Number.isFinite(n))) return null
  if (width <= 0 || height <= 0) return null

  // A window smaller than the app's own minimum would open un-resizable.
  const minWidth = fallback.minWidth ?? 0
  const minHeight = fallback.minHeight ?? 0
  if (width < minWidth || height < minHeight) return null

  const list = Array.isArray(displays) ? displays : []
  const onScreen = list.some((d) => d?.bounds && intersects({ x, y, width, height }, d.bounds))
  if (!onScreen) return null

  return { x, y, width, height }
}

module.exports = { resolveWindowBounds }
