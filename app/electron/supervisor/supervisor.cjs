'use strict'

// ─── Supervisor ─────────────────────────────────────────────────────────────
// Watchdog + crash reporter + recovery manager for the native wallpaper
// runtime and the renderer process.
//
//   Runtime watchdog   — PING heartbeat every 2s; N consecutive misses marks
//                        the runtime hung → minidump + crash report, hard
//                        stop, then normal exit-driven recovery.
//   Renderer watchdog  — the renderer pings every 2s; a stale renderer (while
//                        the window is visible) is crashed/reloaded.
//   Recovery           — runtime exit (crash/kill) triggers a crash report
//                        (plus minidump while the process is alive) and a
//                        replay of the last applied wallpaper via the app's
//                        reapply callback. A circuit breaker stops after
//                        maxRecoveryAttempts consecutive failures.
//   State              — every successful apply is persisted to the StateStore
//                        so recovery survives an Electron restart too.

const { CrashReporter } = require('./crash-reporter.cjs')
const { Minidump } = require('./minidump.cjs')
const { RuntimeManager } = require('./runtime-manager.cjs')
const { StateStore } = require('./state-store.cjs')

const DEFAULTS = {
  heartbeatIntervalMs: 2000,
  pingTimeoutMs: 2500,
  heartbeatMissLimit: 3,
  rendererCheckIntervalMs: 5000,
  rendererStaleMs: 15000,
  maxRecoveryAttempts: 3,
}

class Supervisor {
  // deps: { runtime, stateStore, crashReporter, minidump, logLine,
  //         getWindow, getLastApplied, reapply, setRuntimeAvailable,
  //         onEvent }
  constructor(deps) {
    this.deps = deps
    this.opts = { ...DEFAULTS, ...(deps.options ?? {}) }
    this.manager = deps.runtime instanceof RuntimeManager ? deps.runtime : new RuntimeManager(deps.runtime)
    this.stateStore = deps.stateStore ?? new StateStore(deps.stateFilePath ?? '')
    this.crashReporter = deps.crashReporter ?? new CrashReporter({})
    this.minidump = deps.minidump ?? new Minidump({})

    this.quitting = false
    this.healthy = true
    this.recovering = false
    this.recoveryAttempts = 0
    // Poison-file breaker: a file that crashes the runtime AFTER a
    // successful re-apply resets recoveryAttempts below, so the attempt
    // cap never trips and the same killer file loops forever. Track
    // consecutive crash-after-success per path and park instead.
    this.poisonStreak = 0
    this.lastRecoveredPath = null
    this.heartbeatMisses = 0
    // No renderer ping bridge exists yet (nothing calls recordRendererPing),
    // so the watchdog must stay silent until the first ping arrives —
    // otherwise every visible window would reload in a loop after 15 s.
    this.lastRendererPing = null

    this._timers = []
    this._onExit = (code) => this._handleRuntimeExit(code)
  }

  start() {
    this.deps.logLine(
      'supervisor:start',
      'heartbeat=' + this.opts.heartbeatIntervalMs + 'ms',
      'rendererStale=' + this.opts.rendererStaleMs + 'ms',
      'maxRecoveryAttempts=' + this.opts.maxRecoveryAttempts
    )
    this.manager.on('exit', this._onExit)
    this._every('heartbeat', this.opts.heartbeatIntervalMs, () => this._heartbeatTick())
    this._every('renderer', this.opts.rendererCheckIntervalMs, () => this._rendererTick())
  }

  stop() {
    this.quitting = true
    for (const t of this._timers) clearInterval(t)
    this._timers = []
  }

  // A deliberate new wallpaper apply clears parked recovery state (P0.2):
  // the user has moved on, so a later crash must recover again instead of
  // staying parked on the old wallpaper's failure count.
  resetRecovery() {
    this.recoveryAttempts = 0
    this.poisonStreak = 0
    this.lastRecoveredPath = null
    this.healthy = true
  }

  recordRendererPing() {
    this.lastRendererPing = Date.now()
  }

  handleRendererGone(details) {
    if (this.quitting) return
    const reason = `renderer gone (${details?.reason ?? 'unknown'})`
    this.deps.logLine('supervisor:', reason, 'exitCode', details?.exitCode)
    this._writeReport({ reason, extra: { renderer: details } })
    this._reloadRenderer()
  }

  getStatus() {
    return {
      healthy: this.healthy,
      recovering: this.recovering,
      recoveryAttempts: this.recoveryAttempts,
      maxRecoveryAttempts: this.opts.maxRecoveryAttempts,
      heartbeatMisses: this.heartbeatMisses,
      runtimeAlive: this.manager.alive,
      runtimePid: this.manager.pid,
      lastRendererPing: this.lastRendererPing,
      state: this.stateStore.get(),
    }
  }

  _every(name, ms, fn) {
    const t = setInterval(fn, ms)
    if (t.unref) t.unref()
    this._timers.push(t)
    this.deps.logLine('supervisor:watchdog', name, 'started')
  }

  // ─── Runtime heartbeat ────────────────────────────────────────────────────
  async _heartbeatTick() {
    if (this.quitting || this.recovering || !this.manager.alive) return
    const ok = await this.manager.ping(this.opts.pingTimeoutMs)
    if (this.quitting) return
    if (ok) {
      if (this.heartbeatMisses > 0) this.deps.logLine('supervisor:heartbeat recovered')
      this.heartbeatMisses = 0
      return
    }
    this.heartbeatMisses++
    if (this.heartbeatMisses >= this.opts.heartbeatMissLimit) {
      this.heartbeatMisses = 0
      this._handleRuntimeHung()
    }
  }

  async _handleRuntimeHung() {
    if (this.quitting || this.recovering) return
    const pid = this.manager.pid
    this.deps.logLine('supervisor:runtime hung (no PING x', this.opts.heartbeatMissLimit + ')', 'pid', pid)
    this._writeReport({ reason: 'runtime-hung', pid, minidump: true })
    // Hard stop: SHUTDOWN frame, then the client's 3s kill fallback. A hung
    // process won't answer; killing it fires 'exit' → normal recovery path.
    this.manager.stop()
  }

  // ─── Renderer watchdog ────────────────────────────────────────────────────
  _rendererTick() {
    if (this.quitting) return
    const win = this.deps.getWindow?.()
    if (!win || win.isDestroyed()) return
    // Hidden windows get throttled timers — don't flag a hidden renderer.
    if (!win.isVisible()) return
    // No ping bridge wired yet: without any ping ever recorded there is no
    // baseline, so there is nothing to call stale (see constructor).
    if (this.lastRendererPing === null) return
    const stale = Date.now() - this.lastRendererPing
    if (stale <= this.opts.rendererStaleMs) return
    if (win.webContents.isCrashed()) {
      this.deps.logLine('supervisor:renderer crashed (isCrashed)')
      this._writeReport({ reason: 'renderer-crashed', extra: { staleMs: stale } })
      this._reloadRenderer()
      return
    }
    this.deps.logLine('supervisor:renderer stale', Math.round(stale / 1000) + 's', '- reloading')
    this._writeReport({ reason: 'renderer-stale', extra: { staleMs: stale } })
    this._reloadRenderer()
  }

  _reloadRenderer() {
    const win = this.deps.getWindow?.()
    if (!win || win.isDestroyed()) return
    win.webContents.reload()
    this.lastRendererPing = Date.now()
  }

  // ─── Runtime exit → recovery ──────────────────────────────────────────────
  async _handleRuntimeExit(code) {
    if (this.quitting) return
    const pid = this.manager.pid ?? null
    this.deps.logLine('runtime:exit', code, '(recovering)')
    this.healthy = false
    this.deps.setRuntimeAvailable?.(false)
    this.recovering = true
    this.recoveryAttempts++
    const attempt = this.recoveryAttempts

    this._writeReport({ reason: 'runtime-exit', exitCode: code, pid })

    if (attempt > this.opts.maxRecoveryAttempts) {
      this.deps.logLine('runtime:recovery gave up after', attempt, 'attempts')
      this.healthy = false
      this.recovering = false
      this.deps.setRuntimeAvailable?.(false)
      this._notify({ event: 'recovery-gave-up', data: this.getStatus() })
      return
    }

    // Poison-file circuit breaker (see constructor): reapply success
    // below resets recoveryAttempts, so a file that kills the runtime
    // seconds after every successful re-apply would loop forever.
    const cur = this.deps.getLastApplied?.() ?? this.stateStore.get()
    const curPath = cur?.params?.path ?? null
    const abnormal = code !== 0 && code !== null && code !== undefined
    if (abnormal && curPath && curPath === this.lastRecoveredPath) {
      this.poisonStreak++
    } else {
      this.poisonStreak = 0
    }
    if (this.poisonStreak >= 2 && curPath) {
      this.deps.logLine('runtime:recovery PARKED poison file (crashed', this.poisonStreak + 1, 'times):', curPath)
      this.healthy = false
      this.recovering = false
      this.deps.setRuntimeAvailable?.(false)
      this._notify({ event: 'recovery-parked-poison', data: { path: curPath, streak: this.poisonStreak } })
      return
    }

    this.deps.logLine('runtime:recovery start (last wallpaper queued)', 'attempt', attempt)
    this._notify({ event: 'recovery-start', data: { attempt } })

    try {
      const ok = await this.deps.reapply?.()
      if (ok) {
        this.recoveryAttempts = 0
        this.lastRecoveredPath = curPath
        this.healthy = true
        this.deps.setRuntimeAvailable?.(true)
        this.deps.logLine('runtime:recovery ok (attempt', attempt + ')')
        this._notify({ event: 'recovery-ok', data: { attempt } })
      } else {
        this.deps.logLine('runtime:recovery reapply failed (attempt', attempt + ')')
        this.deps.setRuntimeAvailable?.(false)
      }
    } catch (err) {
      this.deps.logLine('runtime:recovery ERROR', err.message)
    } finally {
      this.recovering = false
    }
  }

  // ─── Crash reporting ──────────────────────────────────────────────────────
  _writeReport({ reason, exitCode, pid, minidump = false }) {
    const state = this.deps.getLastApplied?.() ?? this.stateStore.get()
    const logs = {
      runtimeLog: this.deps.logs?.runtimeLog,
      crashLog: this.deps.logs?.crashLog,
      workerwDiagLog: this.deps.logs?.workerwDiagLog,
      appLog: this.deps.logs?.appLog,
    }
    const report = { reason, exitCode, pid, state, logs }

    if (minidump && pid) {
      // Capture while the process is (possibly) still alive; never block the
      // report on it.
      const dumpDir = this.crashReporter.baseDir
      const dumpPath = require('node:path').join(dumpDir, `runtime-${pid}-${Date.now()}.dmp`)
      this.minidump.capture(pid, dumpPath).then((res) => {
        if (res.ok) report.minidumpPath = res.path
      })
    }

    this.crashReporter.writeReport(report)
  }

  _notify(msg) {
    this.deps.onEvent?.(msg)
  }
}

module.exports = { Supervisor }