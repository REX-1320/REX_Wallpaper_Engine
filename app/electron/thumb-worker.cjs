// GPU thumbnail worker.
//
// The main app runs with `app.disableHardwareAcceleration()` (the UI is a
// settings surface; software compositing keeps it at ~0 MB dedicated VRAM).
// That also removes Chromium's HEVC decoder, so `<video>` poster decoding fails
// for every HEVC file with DEMUXER_ERROR_NO_SUPPORTED_STREAMS. This worker is
// the escape hatch: spawned as a short-lived, hardware-accelerated Electron
// process that decodes ONE poster and exits, so the parked UI keeps its
// zero-VRAM budget.
//
// Protocol (files, not stdio — Electron on Windows has no reliable stdout):
//   electron [appPath] --paiint-thumb-worker <request.json>
//   request  = { path, maxSize, outPath, resultPath }
//   response = { ok, path, reason, ms }
//
// Usage from the app: `poster-worker.cjs`.

const fs = require('node:fs')
const path = require('node:path')

// Candidate timestamps, best first. Sampling a fixed 1 s (the old renderer
// rule) lands on black leader frames for feature-length files; a fifth of the
// way in is representative for loops and skips the opening logos of a movie.
function posterSeekTimes(duration) {
  const d = Number(duration)
  if (!Number.isFinite(d) || d <= 0) return [0.5]
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v))
  const round = (v) => Math.round(v * 1000) / 1000
  const seen = new Set()
  const out = []
  for (const t of [clamp(d * 0.2, 0.5, 600), d * 0.5, d * 0.1, d * 0.7, 0.5]) {
    const v = round(t)
    if (v > 0 && v < d && !seen.has(v)) {
      seen.add(v)
      out.push(v)
    }
  }
  return out
}

// Mean + standard deviation of Rec.709 luma over an RGBA buffer. A uniform
// frame (black leader, white title card) has a deviation near zero, which is
// how the worker knows to try the next timestamp instead of saving it.
function lumaStats(rgba) {
  let sum = 0
  let sumSq = 0
  let n = 0
  for (let i = 0; i + 3 < rgba.length; i += 4) {
    const y = 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2]
    sum += y
    sumSq += y * y
    n += 1
  }
  if (!n) return { mean: 0, std: 0 }
  const mean = sum / n
  return { mean, std: Math.sqrt(Math.max(0, sumSq / n - mean * mean)) }
}

function isFlatFrame(stats) {
  return stats.std < 3 || stats.mean < 2 || stats.mean > 253
}

const WORKER_FLAG = '--paiint-thumb-worker'

function fail(resultPath, reason, startedAt) {
  const result = { ok: false, reason, ms: Date.now() - startedAt }
  try {
    fs.mkdirSync(path.dirname(resultPath), { recursive: true })
    fs.writeFileSync(resultPath, JSON.stringify(result))
  } catch {
    /* nothing else to do */
  }
  return result
}

async function decodePoster(filePath, maxSize) {
  const { app, BrowserWindow } = require('electron')
  await app.whenReady()
  const win = new BrowserWindow({
    show: false,
    width: 640,
    height: 480,
    webPreferences: { webSecurity: false, backgroundThrottling: false },
  })
  try {
    await win.loadURL('data:text/html,<meta charset="utf-8"><body></body>')
    const script = `(async () => {
      const url = ${JSON.stringify('file:///' + String(filePath).split('\\').join('/'))}
      const maxSize = ${maxSize}
      const posterSeekTimes = ${posterSeekTimes.toString()}
      const lumaStats = ${lumaStats.toString()}
      const isFlatFrame = ${isFlatFrame.toString()}
      const canvas = document.createElement('canvas')
      const ctx = canvas.getContext('2d')
      const probe = document.createElement('canvas')
      probe.width = 48
      probe.height = 27
      const probeCtx = probe.getContext('2d', { willReadFrequently: true })
      const once = (target, event, ms) => new Promise((resolve) => {
        let done = false
        const timer = setTimeout(() => { if (!done) { done = true; cleanup(); resolve(false) } }, ms)
        const handler = () => { if (!done) { done = true; cleanup(); resolve(true) } }
        const cleanup = () => { clearTimeout(timer); target.removeEventListener(event, handler) }
        target.addEventListener(event, handler)
      })

      const video = document.createElement('video')
      video.muted = true
      video.playsInline = true
      video.preload = 'metadata'
      const errorReason = () => (video.error && video.error.message) || 'media-error'
      const release = () => {
        try { video.removeAttribute('src'); video.load() } catch {}
      }

      video.src = url
      const metaPromise = Promise.race([
        once(video, 'loadedmetadata', 8000).then((ok) => (ok ? 'ok' : 'timeout')),
        once(video, 'error', 8000).then((ok) => (ok ? 'error' : 'timeout')),
      ])
      video.load()
      const meta = await metaPromise
      if (meta === 'error') { release(); return { ok: false, reason: errorReason() } }
      if (meta !== 'ok') { release(); return { ok: false, reason: 'metadata-timeout' } }

      const duration = Number(video.duration) || 0
      const times = posterSeekTimes(duration)
      for (const t of times) {
        let target = t
        try {
          target = Math.min(Math.max(0.05, t), Math.max(0.05, duration - 0.05))
        } catch {
          continue
        }
        const seekPromise = Promise.race([
          once(video, 'seeked', 5000).then((ok) => (ok ? 'ok' : 'timeout')),
          once(video, 'error', 5000).then((ok) => (ok ? 'error' : 'timeout')),
        ])
        try {
          video.currentTime = target
        } catch {
          continue
        }
        const seeked = await seekPromise
        if (seeked === 'error') { release(); return { ok: false, reason: errorReason() } }
        if (seeked !== 'ok') continue
        const w = video.videoWidth
        const h = video.videoHeight
        if (!w || !h) continue
        const scale = Math.min(1, maxSize / Math.max(w, h))
        canvas.width = Math.max(1, Math.round(w * scale))
        canvas.height = Math.max(1, Math.round(h * scale))
        try {
          ctx.drawImage(video, 0, 0, canvas.width, canvas.height)
          probeCtx.drawImage(video, 0, 0, probe.width, probe.height)
        } catch (err) {
          release()
          return { ok: false, reason: 'draw:' + err.name }
        }
        const stats = lumaStats(probeCtx.getImageData(0, 0, probe.width, probe.height).data)
        if (isFlatFrame(stats)) continue
        let dataUrl = null
        try {
          dataUrl = canvas.toDataURL('image/webp', 0.85)
        } catch {
          dataUrl = null
        }
        if (!dataUrl || dataUrl.length < 200) continue
        release()
        return { ok: true, dataUrl, stats, at: target }
      }
      release()
      return { ok: false, reason: 'no-usable-frame' }
    })()`
    return await win.webContents.executeJavaScript(script)
  } finally {
    try {
      win.destroy()
    } catch {
      /* already gone */
    }
  }
}

async function run(argv) {
  const startedAt = Date.now()
  const flagIndex = argv.indexOf(WORKER_FLAG)
  const requestPath = flagIndex >= 0 ? argv[flagIndex + 1] : null
  if (!requestPath) {
    console.error('thumb-worker: missing request path')
    return 2
  }

  let request = null
  try {
    request = JSON.parse(fs.readFileSync(requestPath, 'utf8'))
  } catch (err) {
    console.error('thumb-worker: bad request:', err.message)
    return 2
  }

  const { path: filePath, maxSize = 480, outPath, resultPath } = request || {}
  if (!filePath || !outPath || !resultPath) {
    fail(resultPath, 'bad-request', startedAt)
    return 2
  }

  try {
    const { app } = require('electron')
    const stat = fs.statSync(filePath)
    if (!stat.isFile()) throw new Error('not a file')

    // The page reads the real duration and derives the sample times from it.
    const shot = await decodePoster(filePath, maxSize)
    if (!shot?.ok || !shot.dataUrl) {
      const result = fail(resultPath, shot?.reason || 'decode-failed', startedAt)
      app.exit(1)
      return 1
    }

    const base64 = String(shot.dataUrl).split(',')[1] || ''
    fs.mkdirSync(path.dirname(outPath), { recursive: true })
    fs.writeFileSync(outPath, Buffer.from(base64, 'base64'))
    const result = { ok: true, path: outPath, bytes: Buffer.byteLength(base64, 'base64'), ms: Date.now() - startedAt }
    fs.writeFileSync(resultPath, JSON.stringify(result))
    app.exit(0)
    return 0
  } catch (err) {
    fail(resultPath, err.message, startedAt)
    try {
      require('electron').app.exit(1)
    } catch {
      /* electron not loaded */
    }
    return 1
  }
}

module.exports = { run, posterSeekTimes, lumaStats, isFlatFrame, WORKER_FLAG }

// Allow `node thumb-worker.cjs ...` to print the policy for quick checks, but
// never auto-run under Electron: main.cjs decides when worker mode starts.
if (require.main === module) {
  console.log(JSON.stringify({
    times10s: posterSeekTimes(10),
    timesMovie: posterSeekTimes(8701),
    flatBlack: isFlatFrame(lumaStats(new Uint8ClampedArray([0, 0, 0, 255]))),
  }, null, 1))
}
