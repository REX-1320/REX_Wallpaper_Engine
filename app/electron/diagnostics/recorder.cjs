'use strict'

// Local-only, crash-safe diagnostics recorder. Every write stays inside
// `rootDir`; nothing is uploaded. The hot path is an append-only JSONL file so
// a crash can lose at most the current line, never the file. Summaries and
// device identity are written atomically (`.tmp` + rename).

const nodeFs = require('node:fs')
const nodePath = require('node:path')

const DEFAULTS = {
  fastMs: 250,
  fastWindowMs: 15_000,
  slowMs: 1000,
  summaryMs: 5000,
  maxBytes: 200 * 1024 * 1024,
  keepSessions: 10,
}

const MAX_LAST_EVENTS = 20
const RECOVERED_PREFIX = 'recovered-'

function noop() {}

class DiagnosticsRecorder {
  constructor(options = {}) {
    if (!options.rootDir) throw new Error('DiagnosticsRecorder: rootDir is required')

    this.fs = options.fsModule || nodeFs
    this.path = options.pathModule || nodePath
    this.now = typeof options.now === 'function' ? options.now : Date.now
    this.log = typeof options.log === 'function' ? options.log : noop

    this.rootDir = this.path.resolve(options.rootDir)
    this.sessionsDir = this.path.join(this.rootDir, 'sessions')
    this.currentFile = this.path.join(this.rootDir, 'current.jsonl')
    this.summaryFile = this.path.join(this.rootDir, 'current-summary.json')
    this.deviceFile = this.path.join(this.rootDir, 'device.json')

    this.getStatus =
      typeof options.getStatus === 'function'
        ? options.getStatus
        : async () => ({ available: false, reason: 'not-configured' })
    this.collectSample =
      typeof options.collectSample === 'function' ? options.collectSample : () => ({})
    this.logFiles = Array.isArray(options.logFiles) ? options.logFiles : []
    this.crashDir = options.crashDir || null
    this.fallback = options.fallback === true
    this.appVersion = options.appVersion ?? null
    this.forced = options.forced === true

    this.maxBytes = options.maxBytes ?? DEFAULTS.maxBytes
    this.keepSessions = options.keepSessions ?? DEFAULTS.keepSessions
    this.fastMs = options.fastMs ?? DEFAULTS.fastMs
    this.fastWindowMs = options.fastWindowMs ?? DEFAULTS.fastWindowMs
    this.slowMs = options.slowMs ?? DEFAULTS.slowMs
    this.summaryMs = options.summaryMs ?? DEFAULTS.summaryMs

    this.started = false
    this.enabled = false
    this.archived = false
    this.capReached = false

    this.sessionName = null
    this.sessionDir = null
    this.startedAt = null

    this.seq = 0
    this.samples = 0
    this.events = 0
    this.bytes = 0
    this.lastSampleAt = null
    this.lastSampleMs = 0
    this.latestSample = null
    this.latestEngine = null
    this.lastEvents = []

    this.fastUntil = 0
    this.samplePromise = null
    this.sampleTimer = null
    this.summaryTimer = null
  }

  // ─── lifecycle ─────────────────────────────────────────────────────────────

  start() {
    if (this.started) return this.status()

    try {
      this._ensureRoot()
      if (!this._recoverPrevious()) {
        this._log('diagnostics: start aborted, previous session could not be recovered')
        return this.status()
      }

      const sessionIso = this._iso()
      this.sessionName = this._uniqueDirName(this._sessionName(sessionIso))
      this.sessionDir = this.path.join(this.sessionsDir, this.sessionName)
      this.fs.mkdirSync(this.sessionDir, { recursive: true })
      // Older builds filed recovered data under recovered-* and left the real
      // session directory empty. Recovery now fills that directory, so any
      // directory still empty at this point is stale. The live session was just
      // created and is skipped by name.
      this._removeEmptySessionDirs(this.sessionName)
      this.fs.writeFileSync(this.currentFile, '')

      this.seq = 0
      this.samples = 0
      this.events = 0
      this.bytes = 0
      this.lastSampleAt = null
      this.lastSampleMs = 0
      this.latestSample = null
      this.latestEngine = null
      this.lastEvents = []
      this.capReached = false
      this.fastUntil = 0
      this.archived = false
      this.started = true
      this.enabled = true

      const sessionRecord = this._append(
        {
          kind: 'session',
          session: this.sessionName,
          appVersion: this.appVersion,
          forced: this.forced,
        },
        sessionIso,
      )
      if (!sessionRecord && !this.capReached) {
        this.enabled = false
        this.started = false
        this._log('diagnostics: start failed, session line not written')
        return this.status()
      }
      this.startedAt = sessionRecord ? sessionRecord.t : sessionIso
      this.writeSummary()
      this._startTimers()
      this.sampleNow().catch((err) => {
        this._log('diagnostics: initial sample failed', this._message(err))
      })
      this._pruneSessions()
      return this.status()
    } catch (err) {
      this._log('diagnostics: start failed', this._message(err))
      this.enabled = false
      this.started = false
      return this.status()
    }
  }

  stop() {
    this._clearTimers()
    this.enabled = false
    return this.status()
  }

  archive(reason = 'manual') {
    if (this.archived) return this.status()
    this.archived = true

    if (!this.started) {
      this._pruneSessions()
      return this.status()
    }

    this.stop()

    const archivedEvent = this._append({
      kind: 'event',
      event: 'archive',
      data: { reason },
    })
    if (archivedEvent) {
      this.events += 1
      this._rememberEvent(archivedEvent)
    }

    this.writeSummary()

    if (this.sessionDir) {
      const sessionFile = this.path.join(this.sessionDir, 'session.jsonl')
      try {
        if (this._exists(this.currentFile)) {
          this._removeIfExists(sessionFile)
          this.fs.renameSync(this.currentFile, sessionFile)
        }
      } catch (err) {
        this._log('diagnostics: archive move failed', this._message(err))
      }

      try {
        if (this._exists(this.summaryFile)) {
          this.fs.copyFileSync(
            this.summaryFile,
            this.path.join(this.sessionDir, 'summary.json'),
          )
        }
      } catch (err) {
        this._log('diagnostics: archive summary failed', this._message(err))
      }

      try {
        if (this._exists(this.deviceFile)) {
          this.fs.copyFileSync(this.deviceFile, this.path.join(this.sessionDir, 'device.json'))
        }
      } catch (err) {
        this._log('diagnostics: archive device failed', this._message(err))
      }
    }

    this._copyLogs()
    this._copyCrashes()
    this._pruneSessions()
    return this.status()
  }

  // ─── public recording API ──────────────────────────────────────────────────

  setDeviceInfo(info) {
    try {
      if (!info || typeof info !== 'object') {
        this._log('diagnostics: device info is not an object')
        return false
      }
      this._ensureRoot()
      this._writeJsonAtomic(this.deviceFile, info)
      return true
    } catch (err) {
      this._log('diagnostics: device info failed', this._message(err))
      return false
    }
  }

  event(name, data) {
    if (!this.enabled || this.archived || this.capReached) return false
    try {
      const record = this._append({
        kind: 'event',
        event: String(name),
        data: data === undefined ? null : data,
      })
      if (!record) return false
      this.events += 1
      this._rememberEvent(record)
      return true
    } catch (err) {
      this._log('diagnostics: event failed', this._message(err))
      return false
    }
  }

  markApply(reason) {
    if (!this.enabled || this.archived || this.capReached) return Promise.resolve(null)
    try {
      this.fastUntil = this.now() + this.fastWindowMs
    } catch (err) {
      this._log('diagnostics: markApply failed', this._message(err))
      return Promise.resolve(null)
    }
    this._log('diagnostics: fast window', reason ?? 'apply')
    return this.sampleNow()
  }

  sampleNow() {
    if (!this.enabled || this.archived || this.capReached) return Promise.resolve(null)
    if (this.samplePromise) return this.samplePromise
    this.samplePromise = this._sampleOnce().finally(() => {
      this.samplePromise = null
    })
    return this.samplePromise
  }

  async _sampleOnce() {
    try {
      let payload = {}
      try {
        const collected = await this.collectSample()
        payload = collected && typeof collected === 'object' ? collected : {}
      } catch (err) {
        payload = { error: this._message(err) }
      }

      let engine
      try {
        engine = await this.getStatus()
      } catch (err) {
        engine = { available: false, reason: this._message(err) }
      }
      if (!engine || typeof engine !== 'object') {
        engine = { available: false, reason: 'no-status' }
      }

      if (!this.enabled || this.archived || this.capReached) return null

      const fields = { ...payload }
      delete fields.t
      delete fields.seq
      delete fields.kind
      const record = this._append({ kind: 'sample', ...fields, engine })
      if (!record) return null

      this.samples += 1
      this.latestSample = payload
      this.latestEngine = engine
      this.lastSampleAt = record.t
      this.lastSampleMs = this.now()
      return record
    } catch (err) {
      this._log('diagnostics: sample failed', this._message(err))
      return null
    }
  }

  writeSummary() {
    let summary = null
    try {
      summary = {
        generatedAt: this._iso(),
        session: {
          startedAt: this.startedAt,
          dir: this.sessionDir,
          appVersion: this.appVersion,
          forced: this.forced,
        },
        counts: {
          samples: this.samples,
          events: this.events,
          bytes: this.bytes,
        },
        latestSample: this.latestSample,
        engine: this.latestEngine,
        lastEvents: this.lastEvents.slice(),
        capReached: this.capReached,
      }
      this._ensureRoot()
      this._writeJsonAtomic(this.summaryFile, summary)
    } catch (err) {
      this._log('diagnostics: summary failed', this._message(err))
    }
    return summary
  }

  status() {
    return {
      enabled: this.enabled,
      dir: this.rootDir,
      sessionDir: this.sessionDir,
      bytes: this.bytes,
      samples: this.samples,
      events: this.events,
      lastSampleAt: this.lastSampleAt,
      capReached: this.capReached,
      fallback: this.fallback,
    }
  }

  // ─── timers ────────────────────────────────────────────────────────────────

  _startTimers() {
    this._clearTimers()

    this.sampleTimer = setInterval(() => this._tick(), this.fastMs)
    this.sampleTimer.unref?.()

    this.summaryTimer = setInterval(() => this.writeSummary(), this.summaryMs)
    this.summaryTimer.unref?.()
  }

  _tick() {
    if (!this.enabled || this.archived || this.capReached) return
    if (this.samplePromise) return

    try {
      const at = this.now()
      if (at < this.fastUntil || at - this.lastSampleMs >= this.slowMs) {
        this.sampleNow().catch((err) => {
          this._log('diagnostics: sample tick failed', this._message(err))
        })
      }
    } catch (err) {
      this._log('diagnostics: tick failed', this._message(err))
    }
  }

  _clearSampleTimer() {
    if (this.sampleTimer) {
      clearInterval(this.sampleTimer)
      this.sampleTimer = null
    }
  }

  _clearTimers() {
    this._clearSampleTimer()
    if (this.summaryTimer) {
      clearInterval(this.summaryTimer)
      this.summaryTimer = null
    }
  }

  // ─── append-only JSONL ─────────────────────────────────────────────────────

  _append(record, at) {
    if (this.capReached) return false
    try {
      const seq = this.seq + 1
      const t = at ?? this._iso()
      const full = { t, seq, ...record }
      // Keep the envelope authoritative even if a sample payload carries t/seq.
      full.t = t
      full.seq = seq
      const line = `${JSON.stringify(full)}\n`
      const lineBytes = Buffer.byteLength(line)

      if (this.bytes + lineBytes > this.maxBytes) {
        this._writeCap()
        return false
      }

      this.fs.appendFileSync(this.currentFile, line)
      this.seq = seq
      this.bytes += lineBytes
      return full
    } catch (err) {
      this._log('diagnostics: append failed', this._message(err))
      return false
    }
  }

  _writeCap() {
    if (this.capReached) return
    this.capReached = true
    this._clearSampleTimer()

    try {
      const seq = this.seq + 1
      const record = {
        t: this._iso(),
        seq,
        kind: 'event',
        event: 'cap-reached',
        data: { bytes: this.bytes },
      }
      const line = `${JSON.stringify(record)}\n`
      this.fs.appendFileSync(this.currentFile, line)
      this.seq = seq
      this.bytes += Buffer.byteLength(line)
      this.events += 1
      this._rememberEvent(record)
      this._log('diagnostics: cap reached', this.bytes)
    } catch (err) {
      this._log('diagnostics: cap write failed', this._message(err))
    }
  }

  _rememberEvent(record) {
    this.lastEvents.push(record)
    if (this.lastEvents.length > MAX_LAST_EVENTS) {
      this.lastEvents.splice(0, this.lastEvents.length - MAX_LAST_EVENTS)
    }
  }

  // ─── crash recovery ────────────────────────────────────────────────────────

  _recoverPrevious() {
    try {
      if (!this._exists(this.currentFile)) return true
      const stat = this.fs.statSync(this.currentFile)
      if (!stat.isFile() || stat.size === 0) return true

      // The first line is the session record written by start(). Filing the
      // data under that recorded name fills the directory the crashed run
      // created instead of leaving it empty and adding a second, misleading
      // recovered-<recovery-time> directory for the same run.
      const recorded = this._readCurrentSessionName()
      const name = this._isSessionDirName(recorded)
        ? recorded
        : this._uniqueDirName(`${RECOVERED_PREFIX}${this._sessionName()}`)
      const dir = this.path.join(this.sessionsDir, name)
      this.fs.mkdirSync(dir, { recursive: true })

      if (this._exists(this.summaryFile)) {
        try {
          this.fs.copyFileSync(this.summaryFile, this.path.join(dir, 'summary.json'))
        } catch (err) {
          this._log('diagnostics: recover summary failed', this._message(err))
        }
      }
      if (this._exists(this.deviceFile)) {
        try {
          this.fs.copyFileSync(this.deviceFile, this.path.join(dir, 'device.json'))
        } catch (err) {
          this._log('diagnostics: recover device failed', this._message(err))
        }
      }

      const destination = this.path.join(dir, 'session.jsonl')
      this._removeIfExists(destination)
      this.fs.renameSync(this.currentFile, destination)
      this._log('diagnostics: recovered previous session', dir)
      return true
    } catch (err) {
      this._log('diagnostics: recover failed', this._message(err))
      return false
    }
  }

  // Read only the head of current.jsonl: the file can be up to maxBytes and
  // the session record is always the first line.
  _readCurrentSessionName() {
    try {
      const fd = this.fs.openSync(this.currentFile, 'r')
      try {
        const buf = Buffer.alloc(64 * 1024)
        const read = this.fs.readSync(fd, buf, 0, buf.length, 0)
        const first = buf.toString('utf8', 0, read).split('\n', 1)[0]
        const record = JSON.parse(first)
        return typeof record?.session === 'string' ? record.session : null
      } finally {
        this.fs.closeSync(fd)
      }
    } catch {
      return null
    }
  }

  _isSessionDirName(name) {
    if (typeof name !== 'string' || name === '') return false
    if (this.path.basename(name) !== name) return false
    return !name.includes('..')
  }

  // Remove directories with no entries. A session that never archived has no
  // files by definition; the live session is created after this runs.
  _removeEmptySessionDirs(keepName) {
    try {
      for (const entry of this.fs.readdirSync(this.sessionsDir, { withFileTypes: true })) {
        if (!entry.isDirectory() || entry.name === keepName) continue
        const dir = this.path.join(this.sessionsDir, entry.name)
        try {
          if (this.fs.readdirSync(dir).length === 0) this.fs.rmdirSync(dir)
        } catch {
          // A concurrent writer (or a locked directory) is not fatal.
        }
      }
    } catch {
      // Missing sessions dir is handled by _ensureRoot on the next start.
    }
  }

  // ─── archive helpers ───────────────────────────────────────────────────────

  _copyLogs() {
    if (!this.sessionDir) return
    for (const entry of this.logFiles) {
      try {
        const source = typeof entry === 'string' ? entry : entry && entry.path
        if (!source || !this._exists(source)) continue
        const rawName =
          typeof entry === 'object' && entry.name ? entry.name : this.path.basename(source)
        const name = this.path.basename(rawName)
        if (!name) continue
        this.fs.copyFileSync(source, this.path.join(this.sessionDir, name))
      } catch (err) {
        this._log('diagnostics: log copy failed', this._message(err))
      }
    }
  }

  _copyCrashes() {
    if (!this.crashDir || !this.sessionDir) return
    try {
      if (!this._exists(this.crashDir)) return
      const destDir = this.path.join(this.sessionDir, 'crashes')
      this.fs.mkdirSync(destDir, { recursive: true })
      for (const entry of this.fs.readdirSync(this.crashDir, { withFileTypes: true })) {
        if (!entry.isFile()) continue
        try {
          this.fs.copyFileSync(
            this.path.join(this.crashDir, entry.name),
            this.path.join(destDir, entry.name),
          )
        } catch (err) {
          this._log('diagnostics: crash copy failed', entry.name, this._message(err))
        }
      }
    } catch (err) {
      this._log('diagnostics: crash scan failed', this._message(err))
    }
  }

  _pruneSessions() {
    try {
      if (!this._exists(this.sessionsDir)) return
      const names = this.fs
        .readdirSync(this.sessionsDir, { withFileTypes: true })
        .filter((entry) => entry.isDirectory())
        .map((entry) => entry.name)

      // The live session is never pruned; it counts against keepSessions.
      const current =
        this.sessionName && names.includes(this.sessionName) ? this.sessionName : null
      const others = names
        .filter((name) => name !== current)
        .sort((a, b) => {
          const keyA = this._pruneKey(a)
          const keyB = this._pruneKey(b)
          if (keyA === keyB) return a < b ? 1 : a > b ? -1 : 0
          return keyA < keyB ? 1 : -1
        })

      const keepOthers = Math.max(0, this.keepSessions - (current ? 1 : 0))
      const keep = new Set(others.slice(0, keepOthers))
      if (current) keep.add(current)

      for (const name of names) {
        if (keep.has(name)) continue
        try {
          this.fs.rmSync(this.path.join(this.sessionsDir, name), {
            recursive: true,
            force: true,
          })
        } catch (err) {
          this._log('diagnostics: prune failed', name, this._message(err))
        }
      }
    } catch (err) {
      this._log('diagnostics: prune scan failed', this._message(err))
    }
  }

  _pruneKey(name) {
    return name.startsWith(RECOVERED_PREFIX) ? name.slice(RECOVERED_PREFIX.length) : name
  }

  // ─── filesystem helpers ────────────────────────────────────────────────────

  _ensureRoot() {
    this.fs.mkdirSync(this.rootDir, { recursive: true })
    this.fs.mkdirSync(this.sessionsDir, { recursive: true })
  }

  _writeJsonAtomic(file, value) {
    const tmp = `${file}.tmp`
    this.fs.writeFileSync(tmp, `${JSON.stringify(value, null, 2)}\n`)
    this.fs.renameSync(tmp, file)
  }

  _exists(file) {
    try {
      return this.fs.existsSync(file)
    } catch {
      return false
    }
  }

  _removeIfExists(file) {
    try {
      this.fs.rmSync(file, { force: true })
    } catch (err) {
      this._log('diagnostics: remove failed', this._message(err))
    }
  }

  _sessionName(at) {
    return (at ?? this._iso()).replace(/[:.]/g, '-')
  }

  _uniqueDirName(base) {
    let name = base
    let suffix = 1
    while (this._exists(this.path.join(this.sessionsDir, name))) {
      suffix += 1
      name = `${base}-${suffix}`
    }
    return name
  }

  _iso() {
    return new Date(this.now()).toISOString()
  }

  _message(err) {
    if (!err) return 'error'
    if (typeof err === 'string') return err
    if (typeof err.message === 'string' && err.message) return err.message
    return String(err)
  }

  _log(...args) {
    try {
      this.log(...args)
    } catch {
      // Logging must never break the recorder.
    }
  }
}

module.exports = { DiagnosticsRecorder }
