// One-time userData migration for the rename to Paiint.
//
// Electron derives the userData directory from the app name, so renaming the
// app silently moves it and every existing library / playlist / setting
// disappears. main.cjs calls this before ANY userData consumer (including the
// thumbnail worker) and pins the path explicitly, so a future rename cannot
// move it again.
//
// Pure fs/path logic (the appData directory is injected) so it runs under plain
// node in tests, the same way startup.cjs is written.

const fs = require('node:fs')
const path = require('node:path')

const TARGET_NAME = 'Paiint'

// Both are real: dev builds derived userData from the package `name` ("pg"),
// packaged builds from productName ("REX").
const SOURCE_NAMES = ['REX', 'pg']

// App-owned state. Electron's caches (GPUCache, Code Cache, Dawn*Cache, ...)
// are deliberately NOT copied: they are regenerated on first launch and are
// keyed to a build that no longer exists.
const STATE_FILES = ['app-state.json', 'wallpaper-library.json', 'last-wallpaper.json']
// `Local Storage` holds the playlist presets/history (engine.js), which are not
// in app-state.json. `thumbnails` avoids re-decoding every poster on upgrade.
const STATE_DIRS = ['config_backups', 'thumbnails', 'Local Storage']

function mtimeOf(dir) {
  try {
    return fs.statSync(path.join(dir, 'app-state.json')).mtimeMs
  } catch {
    return -1
  }
}

// Pick the newest real source. A tester may have both a dev (`pg`) and a
// packaged (`REX`) profile; the one they used last is the one they care about.
function pickSource(appDataDir) {
  let source = null
  let newest = -1
  for (const name of SOURCE_NAMES) {
    const dir = path.join(appDataDir, name)
    const mtime = mtimeOf(dir)
    if (mtime > newest) {
      newest = mtime
      source = dir
    }
  }
  return source
}

/**
 * Copy app-owned state into `<appDataDir>/Paiint`.
 * Idempotent, non-destructive, and never throws: a failed migration must not
 * block startup. The source directory is never deleted, so running the old
 * build remains a working recovery path.
 */
function migrateUserData(appDataDir) {
  const target = path.join(appDataDir, TARGET_NAME)
  const result = { target, source: null, copied: 0, skipped: 0, errors: [] }

  // Already migrated, or a genuinely fresh install.
  if (fs.existsSync(path.join(target, 'app-state.json'))) return result

  const source = pickSource(appDataDir)
  if (!source) return result
  result.source = source

  try {
    fs.mkdirSync(target, { recursive: true })
  } catch (err) {
    result.errors.push(`mkdir: ${err.message}`)
    return result
  }

  for (const file of STATE_FILES) {
    const from = path.join(source, file)
    const to = path.join(target, file)
    try {
      if (!fs.existsSync(from)) continue
      if (fs.existsSync(to)) {
        result.skipped += 1
        continue
      }
      fs.copyFileSync(from, to)
      result.copied += 1
    } catch (err) {
      result.errors.push(`${file}: ${err.message}`)
    }
  }

  for (const name of STATE_DIRS) {
    const from = path.join(source, name)
    const to = path.join(target, name)
    try {
      if (!fs.existsSync(from)) continue
      if (fs.existsSync(to)) {
        result.skipped += 1
        continue
      }
      fs.cpSync(from, to, { recursive: true, force: false, errorOnExist: false })
      result.copied += 1
    } catch (err) {
      result.errors.push(`${name}: ${err.message}`)
    }
  }

  return result
}

module.exports = {
  migrateUserData,
  pickSource,
  TARGET_NAME,
  SOURCE_NAMES,
  STATE_FILES,
  STATE_DIRS,
}
