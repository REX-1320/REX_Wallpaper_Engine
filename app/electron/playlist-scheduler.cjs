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
// How long after our own apply a plan that still carries the PREVIOUS file is
// treated as the renderer not having processed the applied event yet.
const SETTLE_AFTER_APPLY_MS = 2_500
// How long a still image stays on the desktop in a scene-end mode before the
// playlist moves on. A still never emits ENDED, so without this the rotation
// stops dead the first time it reaches one.
const DEFAULT_IMAGE_DWELL_MS = 30_000
const IMAGE_EXTENSIONS = /\.(png|jpe?g|webp|bmp|gif|avif)$/i

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
    // A still image emits no ENDED, so a scene-end mode would park on it
    // forever. Stills therefore get a dwell time (see _applyDwellPlan).
    this.imageDwellMs = Number(options.imageDwellMs) > 0 ? Number(options.imageDwellMs) : DEFAULT_IMAGE_DWELL_MS

    this.plan = null
    this.currentPath = null
    this.nextDueAt = null
    this.timerKey = null
    this.dwellKey = null
    this.timerHandle = null
    this.applying = false
    this.exhausted = false
    this.exhaustedIdentity = null
    this.lastApplyAt = 0
    // The file we rotated AWAY from on our last apply. A plan that still
    // carries it is stale even if it was built a few ms after the apply.
    this.lastAppliedFrom = null
    // Bumped whenever the user's intent changes (new playlist, another file,
    // or a stop). An async native apply that started before the bump must not
    // write its result over the newer intent.
    this.intentGeneration = 0
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
      this.dwellKey = null
      this.exhausted = false
      this.exhaustedIdentity = null
      this.lastAppliedFrom = null
      this.intentGeneration += 1
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
    const playlistChanged = Boolean(this.plan && this.plan.playlistId !== normalized.playlistId)

    // The renderer is authoritative about what is on the desktop, but a plan
    // can arrive while an apply is still settling. Within that window the
    // scheduler's own path wins (it just applied it).
    const incoming = pathKey(normalized.currentPath)
    const current = pathKey(this.currentPath)
    let externalPathChange = false
    if (incoming && incoming !== current) {
      // Plans built before the apply are already rejected above. A plan from
      // the same millisecond as the apply is the one still in flight; the
      // scheduler's own path wins. Anything newer is the renderer reporting a
      // real change (e.g. the user applied another tile).
      const beforeApply = this.lastApplyAt > 0 && updatedAt <= this.lastApplyAt
      // The renderer builds plans every 500 ms. One built a few ms AFTER our
      // apply but before the applied event was processed still carries the
      // file we rotated away from - not a user apply.
      const settlingStale = this.lastApplyAt > 0
        && at - this.lastApplyAt < SETTLE_AFTER_APPLY_MS
        && incoming === pathKey(this.lastAppliedFrom)
      if (!beforeApply && !settlingStale) {
        this.currentPath = normalized.currentPath
        externalPathChange = true
      } else {
        this._log('ignoring plan that predates the last apply', normalized.currentPath)
      }
    } else if (!this.currentPath && incoming) {
      this.currentPath = normalized.currentPath
    }
    normalized.currentPath = this.currentPath ?? null
    if (playlistChanged || externalPathChange) this.intentGeneration += 1

    // A change to the queue/mode/timer is a new intent: let a one-run playlist
    // rotate again after the user edits it.
    const identity = this._planIdentity(normalized)
    if (this.exhausted && this.exhaustedIdentity !== identity) {
      this.exhausted = false
      this.exhaustedIdentity = null
    }

    this.plan = normalized

    if (mode === TIMER_MODE) {
      // A different playlist (or a file applied outside this scheduler) is a
      // fresh start: the interval begins now, even if the renderer's mirrored
      // timer was already overdue. Inheriting that deadline rotated a second
      // member immediately after the user's apply.
      this._applyTimerPlan(normalized, { ...opts, restart: playlistChanged || externalPathChange }, at)
    } else {
      // Scene-end is event-driven, except for still images (no ENDED); every
      // other mode is the renderer's job.
      this._applyDwellPlan(normalized, opts, at)
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
    this.dwellKey = null
    this.exhausted = false
    this.exhaustedIdentity = null
    this.lastAppliedFrom = null
    this.intentGeneration += 1
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

  // What must stay the same for an in-flight native apply to still describe
  // the rotation the user wants. The timer fields are deliberately excluded:
  // the renderer rebroadcasts them, and a rebroadcast is not a new intent.
  _planToken(plan) {
    if (!plan) return 'none'
    return [plan.playlistId ?? '', plan.mode ?? '', this._queueKey(plan)].join('|')
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
    } else if (opts.restart) {
      // A new playlist / a file applied outside this scheduler starts its own
      // interval. The mirrored remainingMs belongs to the previous intent and
      // may already be 0, which would advance past the member the user just
      // applied.
      this.timerKey = key
      this.nextDueAt = at + total
    } else if (key !== this.timerKey || this.nextDueAt == null) {
      this.timerKey = key
      const syncedAt = Number(timer.syncedAt) || at
      const remaining = Number(timer.remainingMs)
      this.nextDueAt = syncedAt + (Number.isFinite(remaining) ? Math.max(0, remaining) : total)
    }

    this._schedule(this.nextDueAt)
  }

  // ─── Still images in scene-end modes ─────────────────────────────────────

  _imageDwellMs(plan) {
    const override = Number(plan?.imageDwellMs)
    if (Number.isFinite(override) && override > 0) return override
    return this.imageDwellMs
  }

  _currentEntry(plan) {
    const index = this._indexOf(this.currentPath ?? plan?.currentPath)
    return index < 0 ? null : plan.queue[index]
  }

  // `apply` is the host's payload, but its `kind` is the one field the
  // scheduler needs: a still image has no end event to wait for.
  _isStill(entry) {
    if (!entry) return false
    const kind = entry.apply?.kind
    if (typeof kind === 'string' && kind) return kind === 'image'
    return IMAGE_EXTENSIONS.test(entry.path ?? '')
  }

  _dwellKey(plan) {
    return [plan?.playlistId ?? '', plan?.mode ?? '', pathKey(this.currentPath ?? plan?.currentPath) ?? ''].join('|')
  }

  _applyDwellPlan(plan, opts, at) {
    const still = SCENE_END_MODES.has(plan.mode) && this._isActive(plan) && this._isStill(this._currentEntry(plan))
    if (!still) {
      this._cancelTimer()
      this.nextDueAt = null
      this.timerKey = null
      this.dwellKey = null
      return
    }

    const key = this._dwellKey(plan)
    if (opts.restoredDueAt != null) {
      this.dwellKey = key
      this.nextDueAt = Number(opts.restoredDueAt)
    } else if (key !== this.dwellKey || this.nextDueAt == null) {
      // Only a new dwell identity re-arms it: the renderer rebroadcasts the
      // plan often, and resetting the clock each time would park on the image
      // just as effectively as having no dwell at all.
      this.dwellKey = key
      this.nextDueAt = at + this._imageDwellMs(plan)
    }

    this.timerKey = null
    this._log('image dwell', `due in ${Math.max(0, this.nextDueAt - at)}ms`, this.currentPath ?? plan.currentPath)
    this._schedule(this.nextDueAt)
  }

  _armDwell(plan, at = this.now()) {
    this.dwellKey = this._dwellKey(plan)
    this.timerKey = null
    this.nextDueAt = at + this._imageDwellMs(plan)
    this._log('image dwell', `due in ${this._imageDwellMs(plan)}ms`, plan.currentPath)
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
    const fromPath = this.currentPath
    const start = index < 0 ? 0 : index + 1
    // The plan can be replaced while the native apply is in flight (the user
    // applies another playlist exactly when the Timer fires). The apply cannot
    // be cancelled, but its result must not update the cursor, arm the old
    // timer or emit a rotation for a plan that no longer exists.
    const token = this._planToken(plan)
    const intent = this.intentGeneration
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
        if (this._planToken(this.plan) !== token || this.intentGeneration !== intent) {
          this._log('plan changed before apply, aborting', `reason=${reason}`)
          return false
        }
        if (!this.exists(candidate)) {
          this._log('skipping unavailable member', candidate.path)
          continue
        }
        try {
          const result = await this.apply(candidate)
          if (result && result.ok === false) {
            throw new Error(result.error ?? 'apply failed')
          }
          if (this._planToken(this.plan) !== token || this.intentGeneration !== intent) {
            this._log('plan changed during apply, discarding result', candidate.path)
            return false
          }
          const activePlan = this.plan
          const at = this.now()
          this.currentPath = candidate.path
          this.lastAppliedFrom = fromPath
          this.lastApplyAt = at
          activePlan.currentPath = candidate.path
          if (activePlan.mode === TIMER_MODE) {
            this._armAfter(activePlan, at)
          } else if (this._isStill(candidate)) {
            // The playlist just landed on a still. It emits no ENDED, so the
            // dwell is the only thing that keeps the rotation moving.
            this._armDwell(activePlan, at)
          } else {
            this._cancelTimer()
            this.nextDueAt = null
            this.timerKey = null
            this.dwellKey = null
          }
          this._emit({
            event: 'applied',
            path: candidate.path,
            reason,
            at,
            playlistId: activePlan.playlistId,
            playlistName: activePlan.playlistName,
            mode: activePlan.mode,
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
      if (this._planToken(this.plan) !== token || this.intentGeneration !== intent) {
        this._log('plan changed while applying, not retrying the old plan', `reason=${reason}`)
        return false
      }
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
    this.dwellKey = null
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
