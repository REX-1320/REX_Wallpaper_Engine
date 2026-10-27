'use strict'

// ─── WallpaperRuntime ──────────────────────────────────────────────────────
// Persistent client for paiint-wallpaper-runtime.exe --runtime.
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
      const wasCurrent = this.proc === proc
      this._teardown(proc, `runtime spawn failed: ${err.message}`)
      // A failed spawn never fires 'exit', so the supervisor used to never
      // hear about it: no crash report, tray stayed "available", no recovery
      // (audit 5.10 #12). Surface it on the same channel as a process exit;
      // code null plus spawnError marks it as a spawn/transport failure.
      if (wasCurrent) this.emit('exit', null, { pid: proc.pid ?? null, spawnError: err.message })
    })

    proc.on('exit', (code) => {
      this.logLine('wallpaper-runtime exited with code', code)
      // Capture the pid before _teardown clears this.proc: the supervisor's
      // crash report read manager.pid afterwards and always got null
      // (audit 5.10 #13).
      const pid = proc.pid ?? null
      // Deliberate idle stops (image applied) must not be reported/replayed
      // as crashes — the supervisor reads this flag (see supervisor.cjs).
      const intentional = proc.__paiintIntentionalStop === true
      if (this.proc === proc) this._teardown(proc, `runtime exited (${code})`)
      this.emit('exit', code, { pid, intentional })
    })

    return proc
  }

  _teardown(proc, reason) {
    // Only the process we currently own may clear client state. A late
    // 'error' from an already-replaced process (e.g. kill() on a dead pid)
    // used to fall through here unconditionally: it rejected the LIVE
    // client's pending requests and dropped its in-flight INIT guard, so the
    // next init() re-sent INIT to an already-initialized engine — exactly
    // what the initPromise guard exists to prevent.
    if (this.proc !== proc) {
      this.logLine('wallpaper-runtime: ignoring teardown from stale process')
      return
    }
    this.proc = null
    this.inited = false
    this.initPromise = null
    for (const { reject } of this.pending.values()) reject(new Error(reason))
    this.pending.clear()
  }

  _onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk])
    while (this.buffer.length >= 4) {
      const len = this.buffer.readUInt32LE(0)
      if (len > MAX_FRAME_BYTES) {
        // A corrupt length prefix means we can no longer tell where frames
        // begin — and anything already buffered is suspect too. Dropping just
        // the buffer left every later command timing out for the rest of the
        // session. Instead, kill the process: the exit handler tears the
        // client down and the normal crash-recovery replays the wallpaper on
        // a fresh, re-synchronized stream. (A runtime that immediately
        // corrupts a fresh stream is bounded by the app's recovery round cap.)
        this.logLine('wallpaper-runtime: oversized frame, restarting runtime (stream out of sync)', len)
        this.buffer = Buffer.alloc(0)
        try {
          if (this.proc) this.proc.kill()
        } catch {
          // already gone
        }
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

  init(maxFps = 0, hardwareAcceleration = true) {
    // Concurrency-safe: concurrent callers share one in-flight INIT so a
    // double-click can never send a second INIT to an initialized engine.
    // protocolVersion: native handshake (P3-G5) — a mismatch fails loudly
    // instead of running on silent defaults.
    if (this.inited) return Promise.resolve()
    if (!this.initPromise) {
      this.initPromise = this.send('INIT', { maxFps, hardwareAcceleration, protocolVersion: 1 })
        .then(() => {
          this.inited = true
        })
        .catch((err) => {
          // A failed INIT (most often the 15 s command timeout) leaves a
          // live process that may still be initializing. Retrying INIT
          // against it answers "already initialized" forever — the client
          // had no reset path and the app wedged until relaunch (audit
          // 5.10 #6). Drop the process so the next attempt spawns fresh.
          const proc = this.proc
          if (proc) {
            this._teardown(proc, `INIT failed: ${err.message}`)
            try {
              proc.kill()
            } catch {
              // already gone
            }
          }
          throw err
        })
        .finally(() => {
          this.initPromise = null
        })
    }
    return this.initPromise
  }

  // Returns a promise that resolves when the native process has actually
  // exited (or the kill deadline fired). Quit must await this: Electron tears
  // its child processes down when the app exits, and a snapshot capture takes
  // seconds, so a fire-and-forget stop truncated the PNG encode and left the
  // desktop unchanged. Other callers may ignore the promise.
  stop(opts = {}) {
    const proc = this.proc
    this.proc = null
    this.inited = false
    if (!proc) return Promise.resolve(null)

    // Tag the process for the 'exit' handler. A deliberate stop is an idle
    // reclaim (static image applied), not a crash: the supervisor must not
    // write a crash report or replay the wallpaper — a replay would spawn a
    // new runtime and undo the reclaim immediately.
    proc.__paiintIntentionalStop = opts.intentional === true

    // Graceful shutdown: write SHUTDOWN directly (NOT via this.send(),
    // which would re-spawn a fresh process) and close stdin. The runtime
    // exits once it sees the frame or EOF.
    try {
      const params = opts.snapshot === true ? { snapshot: true } : {}
      const payload = JSON.stringify({ id: 0xffffffff, cmd: 'SHUTDOWN', params })
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
    // always completes. The deadline covers the native SHUTDOWN sequence
    // (telemetry join ≤250 ms after the unit-11 slicing + engine teardown,
    // GPU drains included); 3 s used to truncate that teardown (audit 5.10
    // #4). Cleared on exit so a clean quit never signals a dead process.
    //
    // A snapshot quit is slower by design: the runtime reads back a
    // full-resolution frame, PNG-encodes it, then applies it through
    // SystemParametersInfo (a synchronous WM_SETTINGCHANGE broadcast that
    // waits for Explorer to load the new wallpaper) before tearing down
    // WorkerW. Measured ~4 s on a 4K desktop; a slower disk or 8K panel can
    // double that. Killing inside that window would abort the capture, so
    // the snapshot path gets a wider deadline — still a guard against a
    // genuinely wedged runtime, never the normal exit path.
    const deadlineMs = opts.snapshot === true ? 15000 : 5000
    return new Promise((resolve) => {
      let settled = false
      const finish = (code) => {
        if (settled) return
        settled = true
        clearTimeout(killTimer)
        resolve(code)
      }
      const killTimer = setTimeout(() => {
        try {
          this.logLine('wallpaper-runtime: no SHUTDOWN response, terminating process')
          proc.kill()
        } catch {
          // already gone
        }
        // Never hang the quit on a process that ignores kill().
        finish(null)
      }, deadlineMs)
      if (killTimer.unref) killTimer.unref()
      proc.once('exit', (code) => finish(code))
      if (proc.exitCode !== null) finish(proc.exitCode)
    })
  }
}

module.exports = { WallpaperRuntime }
