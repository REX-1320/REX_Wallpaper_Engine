'use strict'

// ─── WallpaperRuntime ──────────────────────────────────────────────────────
// Persistent client for rex-wallpaper-cli.exe --runtime.
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

    proc.on('error', (err) => {
      this.logLine('wallpaper-runtime error:', err.message)
      this._teardown(proc, `runtime spawn failed: ${err.message}`)
    })

    proc.on('exit', (code) => {
      this.logLine('wallpaper-runtime exited with code', code)
      if (this.proc === proc) this._teardown(proc, `runtime exited (${code})`)
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
      if (msg.ok) entry.resolve(msg.data ?? null)
      else entry.reject(new Error(msg.error || 'unknown runtime error'))
      return
    }
    if (msg.type === 'event') {
      this.emit('event', msg.event, msg.data ?? {})
      return
    }
  }

  send(cmd, params = {}) {
    return new Promise((resolve, reject) => {
      const id = this.nextId++
      const payload = JSON.stringify({ id, cmd, params })
      this.pending.set(id, { resolve, reject })
      try {
        const proc = this._spawn()
        const byteLength = Buffer.byteLength(payload)
        const frame = Buffer.allocUnsafe(4 + byteLength)
        frame.writeUInt32LE(byteLength, 0)
        frame.write(payload, 4, 'utf8')
        proc.stdin.write(frame, (err) => {
          if (err) {
            this.pending.delete(id)
            reject(err)
          }
        })
      } catch (err) {
        this.pending.delete(id)
        reject(err)
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
    if (!this.proc) return
    this.send('SHUTDOWN').catch(() => {})
    try {
      this.proc.stdin.end()
    } catch {
      // already closed
    }
    this.proc = null
    this.inited = false
  }
}

module.exports = { WallpaperRuntime }
