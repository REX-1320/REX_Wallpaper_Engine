// Startup persistence for REX: remembers the last applied wallpaper across
// app restarts so a login-time launch can re-apply it with no user step.
// Pure fs/path logic (userData dir injected) so it runs under plain node.
const fs = require('node:fs')
const path = require('node:path')

const STATE_FILE = 'last-wallpaper.json'

function statePath(userDataDir) {
  return path.join(userDataDir, STATE_FILE)
}

function isValidEntry(entry) {
  if (!entry || typeof entry !== 'object') return false
  if (entry.kind !== 'image' && entry.kind !== 'video') return false
  if (!entry.params || typeof entry.params.path !== 'string' || !entry.params.path) return false
  return true
}

function saveLastApplied(userDataDir, entry) {
  if (!isValidEntry(entry)) return false
  try {
    fs.mkdirSync(userDataDir, { recursive: true })
    const tmp = `${statePath(userDataDir)}.tmp`
    fs.writeFileSync(tmp, JSON.stringify(entry))
    fs.renameSync(tmp, statePath(userDataDir))
    return true
  } catch {
    return false
  }
}

function loadLastApplied(userDataDir) {
  try {
    const entry = JSON.parse(fs.readFileSync(statePath(userDataDir), 'utf8'))
    return isValidEntry(entry) ? entry : null
  } catch {
    return null
  }
}

function clearLastApplied(userDataDir) {
  try {
    fs.unlinkSync(statePath(userDataDir))
  } catch {}
}

// Descriptor for Electron's app.setLoginItemSettings. Kept here (not in
// main.cjs) so the contract is visible and the shape is unit-testable.
// In a packaged app the executable IS the app, so args suffice. In dev the
// executable is bare Electron and the app folder must be argv[1] — without
// it Windows launches a default Electron window instead of REX.
function loginItemSettings(enabled, appPath) {
  if (appPath) {
    return { openAtLogin: !!enabled, path: process.execPath, args: [appPath, '--background'] }
  }
  return { openAtLogin: !!enabled, args: ['--background'] }
}

module.exports = { saveLastApplied, loadLastApplied, clearLastApplied, loginItemSettings }
