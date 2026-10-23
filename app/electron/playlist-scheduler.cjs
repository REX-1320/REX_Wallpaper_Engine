// Main-process playlist scheduler.
//
// The UI renderer used to own Timer / scene-end rotation, which meant a closed
// window stopped the playlist: the engine lived in a renderer that is destroyed
// to release its GPU memory. This scheduler owns those two triggers instead,
// using a rotation plan mirrored from the renderer (playlist membership + the
// ordered queue + the live settings snapshot). It survives the window being
// destroyed and a `--background` login start, because the main process always
// exists.
//
// The plan is a plain object so it can be persisted in app-state.json:
//
//   { version, playlistId, playlistName, mode, currentPath, loop, paused,
//     updatedAt, timer: { totalMs, remainingMs, running, startedAt,
//     accumulatedMs, syncedAt }, queue: [{ path, apply }] }
//
// `apply` is opaque to the scheduler; the host applies it through the native
// runtime. Timer / scene-end (After Current Ends / Video Ends / Audio Ends)
// are handled here. Every other mode stays in the renderer (those modes use
// simulated signals and were never usable with the window closed anyway).
//
// All side effects are injected (clock, timers, apply, persistence) so the
// scheduling rules are testable without Electron: see playlist-scheduler.test.cjs.

const SCENE_END_MODES = new Set(['After Current Ends', 'Video Ends', 'Audio Ends'])
const TIMER_MODE = 'Timer'
const MAX_TIMER_DELAY = 2_147_483_647 // setTimeout's 32-bit limit (~24.8 days)

function pathKey(value) {
  if (!value) return null
  return String(value).replace(/\//g, '\\').toLowerCase()
}

class PlaylistScheduler {
  constructor(options = {}) {
    this.now = options.now ?? (() => Date.now())
    this.setTimerFn = options.setTimer ?? ((fn, ms) => setTimeout(fn, ms))
    this.clearTimerFn = options.clearTimer ?? ((handle) => clearTimeout(handle))
    this.apply = options.apply
    this.exists = options.exists ?? (() => true)
    this.emit = options.emit ?? (() => {})
    this.persist = options.persist ?? (() => {})
    this.log = options.log ?? (() => {})
    this.retryMs = options.retryMs ?? 60_000

    this.plan = null
    this.currentPath = null
    this.nextDueAt = null
    this.timerKey = null
    this.timerHandle = null
    this.applying = false
    this.exhausted = false
    this.exhaustedIdentity = null
    this.lastApplyAt = 0
    this.lastPersisted = null
    this.started = false
  }

  // ─── Plan lifecycle ──────────────────────────────────────────────────────

  setPlan(plan, opts = {}) {
    const at = this.now()

    if (!plan || !Array.isArray(plan.queue)) {
      this._cancelTimer()
      this.plan = null
      this.currentPath = null
      this.nextDueAt = null
      this.timerKey = null
      this.exhausted = false
      this.exhaustedIdentity = null
      this._persist()
      return this.state()
    }

    const updatedAt = Number(plan.updatedAt) || at
    // A plan built before our last apply describes the pre-rotation desktop.
    // Acting on it would rotate twice (its timer is already overdue) and could
    // resurrect an old mode. Ignore it; the renderer sends a fresh one as soon
    // as it processes the applied event.
    if (this.lastApplyAt > 0 && updatedAt < this.lastApplyAt) {
      this._log('ignoring plan built before the last apply')
      return this.state()
    }

    const queue = (plan.queue ?? []).filter((entry) => entry && typeof entry.path === 'string' && entry.path)
    const mode = plan.mode ?? 'Manual'
    const normalized = {
      ...plan,
      mode,
      loop: plan.loop !== false,
      paused: Boolean(plan.paused),
      queue,
      updatedAt,
    }

    // The renderer is authoritative about what is on the desktop, but a plan
    // can arrive while an apply is still settling. Within that window the
    // scheduler's own path wins (it just applied it).
    const incoming = pathKey(normalized.currentPath)
    const current = pathKey(this.currentPath)
    if (incoming && incoming !== current) {
      // Plans built before the apply are already rejected above. A plan from
      // the same millisecond as the apply is the one still in flight; the
      // scheduler's own path wins. Anything newer is the renderer reporting a
      // real change (e.g. the user applied another tile).
      const inFlight = this.lastApplyAt > 0 && updatedAt === this.lastApplyAt && at - this.lastApplyAt < 2_500
      if (!inFlight) this.currentPath = normalized.currentPath
    } else if (!this.currentPath && incoming) {
      this.currentPath = normalized.currentPath
    }
    normalized.currentPath = this.currentPath ?? null

    // A change to the queue/mode/timer is a new intent: let a one-run playlist
    // rotate again after the user edits it.
    const identity = this._planIdentity(normalized)
    if (this.exhausted && this.exhaustedIdentity !== identity) {
      this.exhausted = false
      this.exhaustedIdentity = null
    }

    this.plan = normalized

    if (mode === TIMER_MODE) {
      this._applyTimerPlan(normalized, opts, at)
    } else {
      // Scene-end is event-driven; every other mode is the renderer's job.
      this._cancelTimer()
      this.nextDueAt = null
      this.timerKey = null
    }

    this._persist()
    return this.state()
  }

  restore(saved) {
    if (!saved || !saved.plan) {
      this._log('restore: no saved rotation')
      return this.state()
    }
    // Seed the dedupe so restoring an identical plan does not rewrite disk.
    this.lastPersisted = JSON.stringify({
      version: 1,
      plan: saved.plan,
      currentPath: saved.currentPath ?? null,
      nextDueAt: saved.nextDueAt ?? null,
      timerKey: saved.timerKey ?? null,
      exhausted: Boolean(saved.exhausted),
    })
    this.currentPath = saved.currentPath ?? saved.plan.currentPath ?? null
    this.exhausted = Boolean(saved.exhausted)
    this.exhaustedIdentity = saved.exhaustedIdentity ?? null
    const plan = { ...saved.plan, currentPath: this.currentPath }
    return this.setPlan(plan, { restoredDueAt: saved.nextDueAt ?? null, restored: true })
  }

  start() {
    this.started = true
    if (this.plan && this.nextDueAt != null && this.plan.mode === TIMER_MODE) {
      this._schedule(this.nextDueAt)
    }
    this._log('started', this.plan ? `mode=${this.plan.mode}` : 'idle')
    return this.state()
  }

  stop() {
    this.started = false
    this._cancelTimer()
    this._log('stopped')
  }

  state() {
    const plan = this.plan
    return {
      active: this._isActive(plan),
      playlistId: plan?.playlistId ?? null,
      playlistName: plan?.playlistName ?? null,
      mode: plan?.mode ?? null,
      currentPath: this.currentPath,
      queueLength: plan?.queue?.length ?? 0,
      nextDueAt: this.nextDueAt,
      exhausted: this.exhausted,
    }
  }

  snapshot() {
    return {
      version: 1,
      plan: this.plan,
      currentPath: this.currentPath,
      nextDueAt: this.nextDueAt,
      timerKey: this.timerKey,
      exhausted: this.exhausted,
      exhaustedIdentity: this.exhaustedIdentity,
    }
  }

  // Called once the boot replay has finished: if the file on the desktop is
  // not part of the persisted plan, rotating that plan would be wrong (the
  // user applied something from another playlist before closing). Drop it and
  // wait for the renderer to sync the real rotation.
  reconcile(path) {
    const plan = this.plan
    if (!plan || !path) return this.state()
    if (this._indexOf(path) >= 0) {
      this.currentPath = path
      plan.currentPath = path
      this._persist()
      return this.state()
    }
    this._log('reconcile: desktop file is outside the saved plan, stopping it', path)
    this._cancelTimer()
    this.plan = null
    this.currentPath = null
    this.nextDueAt = null
    this.timerKey = null
    this.exhausted = false
    this.exhaustedIdentity = null
    this._persist()
    return this.state()
  }

  // ─── Native ENDED ────────────────────────────────────────────────────────

  async handleEvent(event, data) {
    if (event !== 'ENDED') return false
    const plan = this.plan
    if (!plan || !SCENE_END_MODES.has(plan.mode) || plan.paused || !this._isActive(plan)) return false

    const ended = pathKey(data?.path)
    if (!ended) return false
    const current = pathKey(this.currentPath ?? plan.currentPath)
    if (!current) {
      this.currentPath = plan.currentPath ?? null
    } else if (ended !== current) {
      // A stale end from a scene that is already gone.
      this._log('ignoring ENDED for a non-current scene', data?.path)
      return false
    }
    return this._advance(plan.mode)
  }

  // ─── Internals ───────────────────────────────────────────────────────────

  _isActive(plan) {
    if (!plan || plan.paused || this.exhausted) return false
    if (!plan.queue || plan.queue.length === 0) return false
    if (plan.mode === TIMER_MODE) {
      return Boolean(plan.timer?.running) && Number(plan.timer?.totalMs) > 0
    }
    return SCENE_END_MODES.has(plan.mode)
  }

  _planIdentity(plan) {
    return [
      plan.playlistId ?? '',
      plan.mode,
      this._queueKey(plan),
      pathKey(plan.currentPath) ?? '',
      this._timerKey(plan),
    ].join('|')
  }

  _queueKey(plan) {
    return (plan.queue ?? []).map((entry) => pathKey(entry.path)).join('>')
  }

  _timerKey(plan) {
    const timer = plan?.timer
    if (!timer) return 'none'
    return [plan.playlistId ?? '', Number(timer.totalMs) || 0, Number(timer.startedAt) || 0, Number(timer.accumulatedMs) || 0, timer.running ? 1 : 0].join(':')
  }

  _indexOf(path) {
    const target = pathKey(path)
    if (!target || !this.plan) return -1
    const index = this.plan.queue.findIndex((entry) => pathKey(entry.path) === target)
    return index
  }

  _applyTimerPlan(plan, opts, at) {
    const timer = plan.timer ?? {}
    const total = Number(timer.totalMs) || 0
    if (!timer.running || total <= 0 || plan.paused || this.exhausted || plan.queue.length === 0) {
      this._cancelTimer()
      this.nextDueAt = null
      this.timerKey = null
      return
    }

    const key = this._timerKey(plan)
    if (opts.restoredDueAt != null) {
      this.timerKey = key
      this.nextDueAt = Number(opts.restoredDueAt)
    } else if (key !== this.timerKey || this.nextDueAt == null) {
      this.timerKey = key
      const syncedAt = Number(timer.syncedAt) || at
      const remaining = Number(timer.remainingMs)
      this.nextDueAt = syncedAt + (Number.isFinite(remaining) ? Math.max(0, remaining) : total)
    }

    this._schedule(this.nextDueAt)
  }

  _schedule(dueAt) {
    this._cancelTimer()
    const at = this.now()
    this.nextDueAt = dueAt
    const delay = Math.max(0, Number(dueAt) - at)
    if (delay > MAX_TIMER_DELAY) {
      this.timerHandle = this.setTimerFn(() => {
        this.timerHandle = null
        if (this.nextDueAt !== dueAt) return
        this._schedule(dueAt)
      }, MAX_TIMER_DELAY)
      return
    }
    this.timerHandle = this.setTimerFn(() => {
      this.timerHandle = null
      if (this.nextDueAt !== dueAt) return
      this.nextDueAt = null
      return this._advance(TIMER_MODE === this.plan?.mode ? 'Timer elapsed' : this.plan?.mode)
    }, delay)
  }

  _cancelTimer() {
    if (this.timerHandle != null) {
      try {
        this.clearTimerFn(this.timerHandle)
      } catch {}
      this.timerHandle = null
    }
  }

  async _advance(reason) {
    const plan = this.plan
    if (!plan || this.applying || !this._isActive(plan)) return false

    const queue = plan.queue
    const index = this._indexOf(this.currentPath ?? plan.currentPath)
    const start = index < 0 ? 0 : index + 1
    const candidates = []
    if (plan.loop) {
      for (let i = 0; i < queue.length; i += 1) candidates.push(queue[(start + i) % queue.length])
    } else {
      for (let i = start; i < queue.length; i += 1) candidates.push(queue[i])
    }

    if (candidates.length === 0) {
      this._exhaust(plan, reason)
      return false
    }

    // A one-member looping playlist has nowhere to go. Replaying the same
    // file would restart it; the engine's own loop setting owns that behavior.
    if (candidates.length === 1 && this._indexOf(candidates[0].path) === index) {
      if (plan.mode === TIMER_MODE) this._armAfter(plan)
      return false
    }

    this.applying = true
    try {
      for (const candidate of candidates) {
        if (!this.exists(candidate)) {
          this._log('skipping unavailable member', candidate.path)
          continue
        }
        try {
          const result = await this.apply(candidate)
          if (result && result.ok === false) {
            throw new Error(result.error ?? 'apply failed')
          }
          const at = this.now()
          this.currentPath = candidate.path
          this.lastApplyAt = at
          plan.currentPath = candidate.path
          if (plan.mode === TIMER_MODE) this._armAfter(plan, at)
          else {
            this._cancelTimer()
            this.nextDueAt = null
            this.timerKey = null
          }
          this._emit({
            event: 'applied',
            path: candidate.path,
            reason,
            at,
            playlistId: plan.playlistId,
            playlistName: plan.playlistName,
            mode: plan.mode,
            nextDueAt: this.nextDueAt,
          })
          this._log('applied', candidate.path, `reason=${reason}`)
          this._persist()
          return true
        } catch (err) {
          this._log('apply failed', candidate.path, err?.message ?? String(err))
        }
      }

      // Nothing in this cycle could be applied. Keep the playlist alive with
      // a retry instead of silently freezing on the current file.
      this._log('no member could be applied', `reason=${reason}`)
      this._retry(plan, reason)
      this._persist()
      return false
    } finally {
      this.applying = false
    }
  }

  _armAfter(plan, at = this.now()) {
    const total = Number(plan.timer?.totalMs) || 0
    if (total <= 0 || !plan.timer?.running || plan.paused) {
      this._cancelTimer()
      this.nextDueAt = null
      this.timerKey = null
      return
    }
    // Keep the pre-rotation key so a plan the renderer had already built does
    // not recompute an overdue due time; the renderer's post-event plan has a
    // fresh key and re-arms from its reset timer.
    this.timerKey = this._timerKey(plan)
    this._schedule(at + total)
  }

  _retry(plan, reason) {
    this.nextDueAt = this.now() + this.retryMs
    this.timerKey = plan.mode === TIMER_MODE ? this._timerKey(plan) : null
    this._schedule(this.nextDueAt)
    this._log('retry scheduled', `reason=${reason}`, `in=${this.retryMs}ms`)
  }

  _exhaust(plan, reason) {
    this.exhausted = true
    this.exhaustedIdentity = this._planIdentity(plan)
    this._cancelTimer()
    this.nextDueAt = null
    this.timerKey = null
    this._log('playlist complete (one run)', plan.playlistName ?? plan.playlistId ?? '', `reason=${reason}`)
    this._emit({
      event: 'stopped',
      path: this.currentPath,
      reason,
      at: this.now(),
      playlistId: plan.playlistId,
      playlistName: plan.playlistName,
      mode: plan.mode,
      nextDueAt: null,
    })
    this._persist()
  }

  _emit(event) {
    try {
      const result = this.emit(event)
      if (result && typeof result.catch === 'function') result.catch(() => {})
    } catch {}
  }

  _persist() {
    let json = null
    try {
      const snapshot = this.snapshot()
      json = JSON.stringify(snapshot)
      if (json === this.lastPersisted) return
      this.lastPersisted = json
      this.persist(snapshot)
    } catch (err) {
      this._log('persist failed', err?.message ?? String(err))
    }
  }

  _log(...args) {
    try {
      this.log('[playlist-scheduler]', ...args)
    } catch {}
  }
}

module.exports = { PlaylistScheduler, SCENE_END_MODES, TIMER_MODE }
