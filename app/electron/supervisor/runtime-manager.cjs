'use strict'

// ─── RuntimeManager ─────────────────────────────────────────────────────────
// Thin supervisor-side view of the persistent native runtime. The heavy
// lifting (spawn, length-prefixed JSON frame protocol, response correlation,
// event stream, graceful SHUTDOWN + hard-kill fallback) lives in
// wallpaperRuntime.cjs — this wrapper adds the supervisor's needs:
//
//   - ping(): explicit heartbeat via the native PING command
//   - alive / pid accessors for the watchdog and crash reporter

const { WallpaperRuntime } = require('../wallpaperRuntime.cjs')

class RuntimeManager {
  constructor(runtime) {
    // Accept either an existing WallpaperRuntime (normal path) or a cliPath
    // to construct one, so the manager can be used standalone in tests.
    this.runtime = runtime instanceof WallpaperRuntime ? runtime : new WallpaperRuntime(runtime)
  }

  get alive() {
    return !!this.runtime.proc
  }

  get pid() {
    return this.runtime.proc ? this.runtime.proc.pid : null
  }

  // Heartbeat: resolves true when the runtime answers PING within timeoutMs,
  // false on timeout or when the process is gone.
  async ping(timeoutMs = 2500) {
    if (!this.alive) return false
    try {
      await withTimeout(this.runtime.send('PING'), timeoutMs)
      return true
    } catch {
      return false
    }
  }

  // Hard stop: graceful SHUTDOWN frame, then the client's internal 3s
  // hard-kill fallback. Used by the supervisor when the runtime is hung.
  stop() {
    this.runtime.stop()
  }

  send(cmd, params) {
    return this.runtime.send(cmd, params)
  }

  init(maxFps, hardwareAcceleration) {
    return this.runtime.init(maxFps, hardwareAcceleration)
  }

  on(event, listener) {
    this.runtime.on(event, listener)
  }

  emit(event, ...args) {
    this.runtime.emit(event, ...args)
  }
}

function withTimeout(promise, ms) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timeout')), ms)
    promise.then(
      (v) => {
        clearTimeout(timer)
        resolve(v)
      },
      (e) => {
        clearTimeout(timer)
        reject(e)
      }
    )
  })
}

module.exports = { RuntimeManager }