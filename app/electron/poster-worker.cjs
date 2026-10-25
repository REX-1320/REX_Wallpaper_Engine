// Client for the GPU thumbnail worker (see thumb-worker.cjs).
//
// The renderer cannot decode HEVC while the app runs with hardware
// acceleration disabled, so it asks the main process for a poster. This module
// spawns one short-lived hardware-accelerated Electron process per file, waits
// for the WebP it writes, and returns its path. Requests for the same file are
// coalesced, and the spawns are serialised so a gallery restore never opens a
// fleet of GPU processes at once.

const { app } = require('electron')
const { spawn } = require('node:child_process')
const crypto = require('node:crypto')
const fs = require('node:fs')
const path = require('node:path')

const WORKER_FLAG = '--paiint-thumb-worker'
const WORKER_TIMEOUT_MS = 30000

const inFlight = new Map()
let queue = Promise.resolve()

function workDir() {
  return path.join(app.getPath('temp'), 'paiint-thumb-worker')
}

function spawnWorker(filePath, maxSize) {
  return new Promise((resolve) => {
    const dir = workDir()
    const id = crypto.randomUUID()
    const requestPath = path.join(dir, `${id}.request.json`)
    const resultPath = path.join(dir, `${id}.result.json`)
    // Write straight into the poster cache the renderer already reads, so the
    // gallery can persist the path without another copy.
    const outPath = path.join(app.getPath('userData'), 'thumbnails', `${id}.webp`)
    let settled = false
    let child = null
    let timer = null

    const finish = (result) => {
      if (settled) return
      settled = true
      if (timer) clearTimeout(timer)
      for (const p of [requestPath, resultPath]) {
        try {
          fs.rmSync(p, { force: true })
        } catch {
          /* best-effort */
        }
      }
      resolve(result)
    }

    try {
      fs.mkdirSync(dir, { recursive: true })
      fs.writeFileSync(requestPath, JSON.stringify({ path: filePath, maxSize, outPath, resultPath }))
    } catch (err) {
      return finish({ ok: false, reason: 'request-write:' + err.message })
    }

    // A packaged app bakes its entry point into the executable; a development
    // run needs the app directory as the first argument.
    const args = process.defaultApp
      ? [app.getAppPath(), WORKER_FLAG, requestPath]
      : [WORKER_FLAG, requestPath]

    timer = setTimeout(() => {
      try {
        child?.kill()
      } catch {
        /* already gone */
      }
      finish({ ok: false, reason: 'timeout' })
    }, WORKER_TIMEOUT_MS)

    try {
      child = spawn(process.execPath, args, { windowsHide: true, stdio: 'ignore' })
    } catch (err) {
      return finish({ ok: false, reason: 'spawn:' + err.message })
    }

    child.on('error', (err) => finish({ ok: false, reason: 'spawn:' + err.message }))
    child.on('exit', () => {
      let result = null
      try {
        result = JSON.parse(fs.readFileSync(resultPath, 'utf8'))
      } catch {
        result = null
      }
      let bytes = 0
      try {
        bytes = fs.statSync(outPath).size
      } catch {
        bytes = 0
      }
      if (result?.ok && bytes > 200) {
        return finish({ ok: true, path: outPath, bytes, ms: result.ms ?? null })
      }
      try {
        fs.rmSync(outPath, { force: true })
      } catch {
        /* nothing to remove */
      }
      finish({ ok: false, reason: result?.reason ?? 'worker-failed' })
    })
  })
}

// Resolves { ok: true, path } with a WebP poster on disk, or { ok: false, reason }.
function generatePoster(filePath, maxSize = 480) {
  const key = `${String(filePath).toLowerCase()}|${maxSize}`
  const existing = inFlight.get(key)
  if (existing) return existing

  const run = queue
    .catch(() => {})
    .then(() => spawnWorker(filePath, maxSize))
    .finally(() => inFlight.delete(key))
  queue = run.catch(() => {})
  inFlight.set(key, run)
  return run
}

module.exports = { generatePoster, WORKER_FLAG, WORKER_TIMEOUT_MS }
