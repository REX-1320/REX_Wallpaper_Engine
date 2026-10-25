// One-time cleanup of the pre-rename Windows login item.
//
// setLoginItemSettings keys the HKCU Run entry by app name, so after the rename
// the old "REX" entry survives and points at the old executable. Windows would
// then start BOTH builds at sign-in and two UIs would fight over the desktop.
// Remove only entries whose command line points at an old build; every other
// Run entry is left untouched.
//
// Windows-only and fully guarded: a failed cleanup must never block startup.

const { execFileSync } = require('node:child_process')

const RUN_KEY = 'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run'

// `reg query` prints one value per line as:
//     Name    REG_SZ    C:\path\app.exe --background
function parseRunValues(output) {
  const values = []
  for (const line of String(output).split(/\r?\n/)) {
    const match = line.match(/^\s{4}(\S.*?)\s{2,}REG_\w+\s{2,}(.*)$/)
    if (match) values.push({ name: match[1], command: match[2].trim() })
  }
  return values
}

function isLegacyCommand(command) {
  const lower = String(command).toLowerCase()
  return /(^|[\\/])rex[^\\/\s]*\.exe(\s|$)/.test(lower)
}

function cleanupLegacyLoginItem({
  platform = process.platform,
  exec = execFileSync,
  log = () => {},
} = {}) {
  if (platform !== 'win32') return { removed: [] }

  let output
  try {
    output = exec('reg', ['query', RUN_KEY], { encoding: 'utf8', windowsHide: true })
  } catch {
    return { removed: [] } // no Run key yet, or reg unavailable
  }

  const removed = []
  for (const value of parseRunValues(output)) {
    if (!isLegacyCommand(value.command)) continue
    try {
      exec('reg', ['delete', RUN_KEY, '/v', value.name, '/f'], { windowsHide: true })
      removed.push(value.name)
      log(`[login-item] removed stale entry "${value.name}" -> ${value.command}`)
    } catch (err) {
      log(`[login-item] could not remove "${value.name}": ${err.message}`)
    }
  }
  return { removed }
}

module.exports = { cleanupLegacyLoginItem, parseRunValues, isLegacyCommand, RUN_KEY }
