'use strict'

// The occlusion monitor (PowerShell) prints exactly one state line whenever the
// desktop visibility changes:
//
//   CLEAR
//   OCCLUDED <class> <width>x<height>
//
// Everything else (Add-Type chatter, warnings) is noise and must never flip the
// mute state. The parser is pure so the wire format is pinned by tests.
function parseOcclusionLine(line) {
  if (typeof line !== 'string') return null
  const text = line.trim()
  if (text === 'CLEAR') return { occluded: false, detail: '' }
  if (text === 'OCCLUDED') return { occluded: true, detail: '' }
  if (text.startsWith('OCCLUDED ')) {
    return { occluded: true, detail: text.slice('OCCLUDED '.length).trim() }
  }
  return null
}

module.exports = { parseOcclusionLine }
