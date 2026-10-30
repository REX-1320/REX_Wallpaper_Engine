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

module.exports = { isWindowOpen, effectiveMute }
