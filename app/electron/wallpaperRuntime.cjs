'use strict'

// ─── WallpaperRuntime ──────────────────────────────────────────────────────
// Persistent client for rex-wallpaper-runtime.exe --runtime.
//
// The runtime process lives for the lifetime of the app. Wallpaper applies
// are fast LOAD/PLAY commands instead of kill+respawn cycles, and the
// engine (D3D12, MediaFoundation, decoder) stays warm between swaps.
//
// Transport: length-prefixed UTF-8 JSON frames on stdin/stdout.
//   [uint32 LE length][payload]

const { spawn } = require('node:child_process')
const { EventEmitter } = require('node:events')

const MAX_FRAME_BYTES = 16 * 1024 * 1024

class WallpaperRuntime extends EventEmitter {
  constructor(cliPath, logLine = () => {}) {
    super()
    this.cliPath = cliPath
    this.logLine = logLine
    this.proc = null
    this.nextId = 1
    this.pending = new Map()
    this.buffer = Buffer.alloc(0)
    this.inited = false
    this.initPromise = null
  }

  _spawn() {
    if (this.proc) return this.proc
    const proc = spawn(this.cliPath, ['--runtime'], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    })
    this.proc = proc
    this.buffer = Buffer.alloc(0)

    proc.stdout.on('data', (chunk) => this._onData(chunk))

    // Drain stderr: it is never read by the protocol, and an unread pipe
    // fills up (64 KB) and blocks the runtime's writes — a latent freeze
    // source. Forward it to the app log (capped per chunk to avoid spam).
    proc.stderr.on('data', (chunk) => {
      const text = chunk.toString('utf8').trim()
      if (text) this.logLine('runtime[stderr]:', text.slice(0, 2000))
    })

    proc.on('error', (err) => {
      this.logLine('wallpaper-runtime error:', err.message)
      this._teardown(proc, `runtime spawn failed: ${err.message}`)
    })

    proc.on('exit', (code) => {
      this.logLine('wallpaper-runtime exited with code', code)
      if (this.proc === proc) this._teardown(proc, `runtime exited (${code})`)
      this.emit('exit', code)
    })

    return proc
  }

  _teardown(proc, reason) {
    if (this.proc === proc) {
      this.proc = null
      this.inited = false
    }
    this.initPromise = null
    for (const { reject } of this.pending.values()) reject(new Error(reason))
    this.pending.clear()
  }

  _onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk])
    while (this.buffer.length >= 4) {
      const len = this.buffer.readUInt32LE(0)
      if (len > MAX_FRAME_BYTES) {
        this.logLine('wallpaper-runtime: oversized frame, dropping', len)
        this.buffer = Buffer.alloc(0)
        return
      }
      if (this.buffer.length < 4 + len) return
      const payload = this.buffer.subarray(4, 4 + len).toString('utf8')
      this.buffer = this.buffer.subarray(4 + len)
      let msg
      try {
        msg = JSON.parse(payload)
      } catch {
        continue
      }
      this._handleMessage(msg)
    }
  }

  _handleMessage(msg) {
    if (msg.type === 'response') {
      const entry = this.pending.get(msg.id)
      if (!entry) return
      this.pending.delete(msg.id)
      if (entry.timer) clearTimeout(entry.timer)
      if (msg.ok) entry.resolve(msg.data ?? null)
      else entry.reject(new Error(msg.error || 'unknown runtime error'))
      return
    }
    if (msg.type === 'event') {
      this.emit('event', msg.event, msg.data ?? {})
      return
    }
  }

  // Default per-command timeout: a wedged native runtime must reject the
  // caller (so the UI can desync-flag) instead of leaking the promise and
  // stalling every later apply queued behind it. SHUTDOWN bypasses send().
  static DEFAULT_TIMEOUT_MS = 15000;

  send(cmd, params = {}, timeoutMs = WallpaperRuntime.DEFAULT_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
      const id = this.nextId++
      const payload = JSON.stringify({ id, cmd, params })
      const entry = { resolve, reject, timer: null }
      this.pending.set(id, entry)
      if (timeoutMs > 0) {
        entry.timer = setTimeout(() => {
          if (this.pending.delete(id)) {
            this.logLine(`wallpaper-runtime: '${cmd}' timed out after ${timeoutMs}ms`)
            reject(new Error(`runtime timeout: ${cmd}`))
          }
        }, timeoutMs)
        if (entry.timer.unref) entry.timer.unref()
      }
      const settle = (fn, arg) => {
        if (!this.pending.has(id)) return // already timed out
        this.pending.delete(id)
        if (entry.timer) clearTimeout(entry.timer)
        fn(arg)
      }
      try {
        const proc = this._spawn()
        const byteLength = Buffer.byteLength(payload)
        const frame = Buffer.allocUnsafe(4 + byteLength)
        frame.writeUInt32LE(byteLength, 0)
        frame.write(payload, 4, 'utf8')
        proc.stdin.write(frame, (err) => {
          if (err) settle(reject, err)
        })
      } catch (err) {
        settle(reject, err)
      }
    })
  }

  init(maxFps = 60, hardwareAcceleration = true) {
    // Concurrency-safe: concurrent callers share one in-flight INIT so a
    // double-click can never send a second INIT to an initialized engine.
    if (this.inited) return Promise.resolve()
    if (!this.initPromise) {
      this.initPromise = this.send('INIT', { maxFps, hardwareAcceleration })
        .then(() => {
          this.inited = true
        })
        .finally(() => {
          this.initPromise = null
        })
    }
    return this.initPromise
  }

  stop() {
    const proc = this.proc
    this.proc = null
    this.inited = false
    if (!proc) return

    // Graceful shutdown: write SHUTDOWN directly (NOT via this.send(),
    // which would re-spawn a fresh process) and close stdin. The runtime
    // exits once it sees the frame or EOF.
    try {
      const payload = JSON.stringify({ id: 0xffffffff, cmd: 'SHUTDOWN', params: {} })
      const frame = Buffer.allocUnsafe(4 + Buffer.byteLength(payload))
      frame.writeUInt32LE(Buffer.byteLength(payload), 0)
      frame.write(payload, 4, 'utf8')
      proc.stdin.write(frame)
    } catch {
      // stdin already closed
    }
    try {
      proc.stdin.end()
    } catch {
      // already closed
    }

    // Hard fallback: a wedged runtime (e.g. render-loop freeze) never
    // answers SHUTDOWN and never exits on EOF. Its open pipes would keep
    // the Electron app alive forever with no window — kill it so app quit
    // always completes.
    const killTimer = setTimeout(() => {
      try {
        this.logLine('wallpaper-runtime: no SHUTDOWN response, terminating process')
        proc.kill()
      } catch {
        // already gone
      }
    }, 3000)
    if (killTimer.unref) killTimer.unref()
  }
}

module.exports = { WallpaperRuntime }
