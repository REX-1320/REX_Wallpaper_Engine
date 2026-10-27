// MSIX startup task bridge.
//
// A packaged app cannot use app.setLoginItemSettings: its HKCU Run write is
// redirected into the package hive, so Windows never sees it. The manifest
// declares a windows.startupTask instead, and only a process with package
// identity can enable/disable it. `paiint-startup.exe` is that process (and
// also the logon stub, because the documented startupTask form takes no
// arguments).
//
// The helper prints one state token; execFileSync throws on a non-zero exit,
// so the token is read from err.stdout in that case.
const { execFileSync } = require('node:child_process')

const STATES = new Set([
  'enabled',
  'disabled',
  'disabled-by-user',
  'disabled-by-policy',
  'enabled-by-policy',
])

// The helper exists but never produced a state (blocked, timed out, crashed).
// Distinct from null, which means "no package identity, use the Run key".
const UNAVAILABLE = 'unavailable'

function parseState(stdout) {
  const text = String(stdout ?? '').trim()
  // No output at all: the helper is missing or crashed, so there is no
  // startup task to consult. The caller must fall back to the Run key
  // (this is the portable build with no stub on disk).
  if (!text) return null
  const token = text.split(/\s+/)[0].toLowerCase()
  if (token === 'unpackaged') return null
  if (STATES.has(token)) return token
  return 'error'
}

function query(stubPath) {
  if (!stubPath) return null
  try {
    const raw = execFileSync(stubPath, ['--status'], {
      windowsHide: true,
      timeout: 3000,
      encoding: 'utf8',
    })
    const state = parseState(raw)
    if (state !== null) return state
    // parseState only returns null for "unpackaged" or no output. The token is
    // the legitimate portable answer; silence means the helper never ran.
    return String(raw ?? '').trim() ? null : UNAVAILABLE
  } catch (err) {
    // A stub that is not on disk is the normal portable/dev case.
    if (err?.code === 'ENOENT') return null
    const state = parseState(err?.stdout)
    if (state !== null) return state
    return String(err?.stdout ?? '').trim() ? null : UNAVAILABLE
  }
}

function setEnabled(stubPath, enabled) {
  if (!stubPath) return 'error'
  try {
    return parseState(
      execFileSync(stubPath, [enabled ? '--enable' : '--disable'], {
        windowsHide: true,
        // RequestEnableAsync can wait on the shell; still bounded so the
        // Settings toggle can never hang the main process.
        timeout: 8000,
        encoding: 'utf8',
      }),
    )
  } catch (err) {
    return parseState(err?.stdout)
  }
}

function isEnabled(state) {
  return state === 'enabled' || state === 'enabled-by-policy'
}

module.exports = { parseState, query, setEnabled, isEnabled, UNAVAILABLE }
