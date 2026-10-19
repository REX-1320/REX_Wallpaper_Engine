'use strict'

// ─── Path containment guard ─────────────────────────────────────────────────
// IPC handlers that delete files must only ever touch paths inside their own
// directory. Windows paths are case-insensitive, so a bare startsWith() lets
// both sibling directories ('C:\x\thumbs-evil' vs 'C:\x\thumbs') and
// case-variant paths ('C:\X\THUMBS\...') through (audit 5.10 #14). Resolve
// both sides, require the separator and compare case-insensitively.

const path = require('node:path')

function isPathInside(childPath, parentDir) {
  if (typeof childPath !== 'string' || typeof parentDir !== 'string') return false
  if (!childPath || !parentDir) return false
  const dir = path.resolve(parentDir) + path.sep
  const child = path.resolve(childPath)
  return child.toLowerCase().startsWith(dir.toLowerCase())
}

module.exports = { isPathInside }
