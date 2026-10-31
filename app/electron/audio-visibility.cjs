'use strict'

// A Paiint window is "open" only while it is actually on screen. A window
// hidden by close-to-tray, destroyed on close, minimized, or never created
// (--background start) does not count: the user cannot see the app, so the
// wallpaper may speak.
function isWindowOpen(win) {
  if (!win || typeof win.isDestroyed !== 'function') return false
  if (win.isDestroyed()) return false
  if (typeof win.isVisible === 'function' && !win.isVisible()) return false
  if (typeof win.isMinimized === 'function' && win.isMinimized()) return false
  return true
}

// The mute value actually sent to the engine. `intentMuted` is the user's
// per-wallpaper choice; `onlyWhenClosed` is the Settings override. The
// override can only ADD mute - it never unmutes a wallpaper the user muted.
function effectiveMute({ intentMuted, onlyWhenClosed, windowOpen } = {}) {
  if (intentMuted === true) return true
  if (onlyWhenClosed === true && windowOpen === true) return true
  return false
}

// Which signal decides whether the desktop counts as covered. Until the
// occlusion monitor has reported once, the Paiint window is the only signal we
// trust (mute - the safe default). After that the monitor wins: it enumerates
// every top-level window (Paiint's included) and knows when the desktop is
// genuinely visible again, e.g. Win+D raises the desktop above every window
// while isWindowOpen() still says true because nothing was minimized.
function desktopCovered({ occlusionReported, desktopOccluded, windowOpen } = {}) {
  if (occlusionReported === true) return desktopOccluded === true
  return windowOpen === true
}

module.exports = { isWindowOpen, effectiveMute, desktopCovered }
