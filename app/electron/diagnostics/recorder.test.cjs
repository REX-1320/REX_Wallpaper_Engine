'use strict'

const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')

const { DiagnosticsRecorder } = require('./recorder.cjs')

// ─── fixtures ────────────────────────────────────────────────────────────────

const FIXED_NOW = Date.UTC(2026, 9, 30, 12, 0, 0)

const SAMPLE = {
  app: { uptimeSec: 12, memory: { rss: 1000 } },
  window: {
    exists: true,
    open: true,
    visible: true,
    minimized: false,
    focused: true,
    bounds: { x: 0, y: 0, width: 800, height: 600 },
  },
  displays: [{ id: 1, scaleFactor: 1 }],
  power: { onBattery: false, idleTime: 0, thermalState: 'nominal' },
  lastApplied: { kind: 'video', path: 'C:\\media\\clip.mp4' },
  playlist: null,
  audio: {
    onlyWhenClosed: false,
    intentMuted: true,
    windowOpen: true,
    effectiveMuted: false,
  },
}

const DEVICE = {
  app: { name: 'Paiint', version: '0.1.0' },
  os: { platform: 'win32' },
  displays: [],
}

// ─── helpers ─────────────────────────────────────────────────────────────────

function tempRoot(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'paiint-diag-'))
  t.after(() => fs.rmSync(root, { recursive: true, force: true }))
  return root
}

function readJsonLines(file) {
  return fs
    .readFileSync(file, 'utf8')
    .split('\n')
    .filter((line) => line.trim() !== '')
    .map((line) => JSON.parse(line))
}

function makeRecorder(root, overrides = {}) {
  const { clock = FIXED_NOW, ...options } = overrides
  return new DiagnosticsRecorder({
    rootDir: root,
    now: () => clock,
    collectSample: () => SAMPLE,
    getStatus: async () => ({ available: true, data: { fps: 60 } }),
    log: () => {},
    // Keep the periodic timers out of the way unless a test opts in.
    fastMs: 60_000,
    summaryMs: 60_000,
    ...options,
  })
}

function startRecorder(t, root, overrides = {}) {
  const recorder = makeRecorder(root, overrides)
  t.after(() => recorder.stop())
  recorder.start()
  return recorder
}

// ─── lifecycle ───────────────────────────────────────────────────────────────

test('start creates current files, a session directory and a session line', (t) => {
  const root = tempRoot(t)
  startRecorder(t, root)

  assert.ok(fs.existsSync(path.join(root, 'current.jsonl')))
  assert.ok(fs.existsSync(path.join(root, 'current-summary.json')))
  assert.equal(fs.existsSync(path.join(root, 'device.json')), false)

  const sessions = fs.readdirSync(path.join(root, 'sessions'), { withFileTypes: true })
  assert.equal(sessions.length, 1)
  assert.ok(sessions[0].isDirectory())

  const lines = readJsonLines(path.join(root, 'current.jsonl'))
  assert.equal(lines[0].kind, 'session')
  assert.equal(lines[0].seq, 1)
  assert.equal(lines[0].session, sessions[0].name)
  assert.equal(lines[0].session, lines[0].t.replace(/[:.]/g, '-'))
  assert.match(lines[0].t, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/)

  const summary = JSON.parse(fs.readFileSync(path.join(root, 'current-summary.json'), 'utf8'))
  assert.equal(summary.session.dir, path.join(root, 'sessions', sessions[0].name))
  assert.equal(summary.session.startedAt, lines[0].t)
})

test('event appends one parseable JSON line with the event name and data', (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root)

  assert.equal(recorder.event('apply', { path: 'x' }), true)

  const lines = readJsonLines(path.join(root, 'current.jsonl'))
  const matches = lines.filter((line) => line.kind === 'event' && line.event === 'apply')
  assert.equal(matches.length, 1)
  assert.deepEqual(matches[0].data, { path: 'x' })
  assert.ok(matches[0].seq > 1)
  assert.match(matches[0].t, /^\d{4}-\d{2}-\d{2}T/)
})

test('event is a no-op before start and after archive', (t) => {
  const root = tempRoot(t)
  const recorder = makeRecorder(root)
  t.after(() => recorder.stop())

  assert.equal(recorder.event('before-start', {}), false)
  assert.equal(fs.existsSync(path.join(root, 'current.jsonl')), false)

  recorder.start()
  recorder.archive('test')
  const size = fs.statSync(path.join(recorder.status().sessionDir, 'session.jsonl')).size
  assert.equal(recorder.event('after-archive', {}), false)
  assert.equal(fs.statSync(path.join(recorder.status().sessionDir, 'session.jsonl')).size, size)
})

test('timers are unref\'d and cleared on stop', (t) => {
  const root = tempRoot(t)
  const recorder = makeRecorder(root)
  recorder.start()

  assert.equal(recorder.sampleTimer.hasRef(), false)
  assert.equal(recorder.summaryTimer.hasRef(), false)

  recorder.stop()
  assert.equal(recorder.sampleTimer, null)
  assert.equal(recorder.summaryTimer, null)
})

// ─── samples ─────────────────────────────────────────────────────────────────

test('sampleNow appends a sample with the collectSample payload and getStatus result', async (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root, {
    collectSample: () => SAMPLE,
    getStatus: async () => ({ available: true, data: { fps: 60 } }),
  })

  await recorder.sampleNow()

  const samples = readJsonLines(path.join(root, 'current.jsonl')).filter(
    (line) => line.kind === 'sample',
  )
  assert.ok(samples.length >= 1)
  const sample = samples.at(-1)
  assert.deepEqual(sample.app, SAMPLE.app)
  assert.deepEqual(sample.window, SAMPLE.window)
  assert.deepEqual(sample.displays, SAMPLE.displays)
  assert.deepEqual(sample.power, SAMPLE.power)
  assert.deepEqual(sample.lastApplied, SAMPLE.lastApplied)
  assert.deepEqual(sample.audio, SAMPLE.audio)
  assert.deepEqual(sample.engine, { available: true, data: { fps: 60 } })
  assert.ok(sample.seq > 1)
})

test('getStatus rejection is recorded as unavailable and does not throw', async (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root, {
    getStatus: async () => {
      throw new Error('runtime gone')
    },
  })

  await assert.doesNotReject(() => recorder.sampleNow())

  const sample = readJsonLines(path.join(root, 'current.jsonl'))
    .filter((line) => line.kind === 'sample')
    .at(-1)
  assert.deepEqual(sample.engine, { available: false, reason: 'runtime gone' })
})

test('sampleNow returns the in-flight sample instead of overlapping it', async (t) => {
  const root = tempRoot(t)
  let calls = 0
  const recorder = startRecorder(t, root, {
    collectSample: async () => {
      calls += 1
      return SAMPLE
    },
  })

  const [first, second] = await Promise.all([recorder.sampleNow(), recorder.sampleNow()])
  assert.equal(first, second)
  assert.equal(calls, 1)
})

// ─── device + summary ────────────────────────────────────────────────────────

test('setDeviceInfo writes valid JSON to device.json atomically', (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root)

  assert.equal(recorder.setDeviceInfo(DEVICE), true)

  const text = fs.readFileSync(path.join(root, 'device.json'), 'utf8')
  assert.deepEqual(JSON.parse(text), DEVICE)
  assert.equal(fs.existsSync(path.join(root, 'device.json.tmp')), false)

  const updated = { ...DEVICE, app: { name: 'Paiint', version: '0.1.1' } }
  assert.equal(recorder.setDeviceInfo(updated), true)
  assert.deepEqual(
    JSON.parse(fs.readFileSync(path.join(root, 'device.json'), 'utf8')),
    updated,
  )
  assert.equal(fs.existsSync(path.join(root, 'device.json.tmp')), false)
})

test('writeSummary records counts, latest sample, last 20 events and cap state', async (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root, {
    collectSample: () => SAMPLE,
    getStatus: async () => ({ available: true, data: { fps: 60 } }),
  })

  await recorder.sampleNow() // the sample start() already kicked off
  await recorder.sampleNow() // a second, fresh sample
  for (let i = 0; i < 25; i += 1) {
    assert.equal(recorder.event(`event-${i}`, { i }), true)
  }

  const summary = recorder.writeSummary()
  const onDisk = JSON.parse(fs.readFileSync(path.join(root, 'current-summary.json'), 'utf8'))

  assert.deepEqual(onDisk, summary)
  assert.equal(onDisk.counts.samples, 2)
  assert.equal(onDisk.counts.events, 25)
  assert.ok(onDisk.counts.bytes > 0)
  assert.deepEqual(onDisk.latestSample, SAMPLE)
  assert.deepEqual(onDisk.engine, { available: true, data: { fps: 60 } })
  assert.equal(onDisk.lastEvents.length, 20)
  assert.equal(onDisk.lastEvents[0].event, 'event-5')
  assert.equal(onDisk.lastEvents[19].event, 'event-24')
  assert.equal(onDisk.capReached, false)
  assert.match(onDisk.generatedAt, /^\d{4}-\d{2}-\d{2}T/)
  assert.equal(fs.existsSync(path.join(root, 'current-summary.json.tmp')), false)
})

// ─── archive + prune ─────────────────────────────────────────────────────────

test('archive moves the session and copies device, summary, logs and crash dumps', (t) => {
  const root = tempRoot(t)
  const logDir = path.join(root, 'logs')
  const crashDir = path.join(root, 'crashes')
  fs.mkdirSync(logDir, { recursive: true })
  fs.mkdirSync(crashDir, { recursive: true })

  const runtimeLog = path.join(logDir, 'runtime.log')
  const appLog = path.join(logDir, 'app.log')
  const missingLog = path.join(logDir, 'missing.log')
  fs.writeFileSync(runtimeLog, 'runtime line\n')
  fs.writeFileSync(appLog, 'app line\n')
  fs.writeFileSync(path.join(crashDir, 'crash-1.dmp'), 'dump-bytes')
  fs.writeFileSync(path.join(crashDir, 'crash-2.dmp'), 'dump-bytes-2')
  fs.writeFileSync(path.join(crashDir, 'notes.txt'), 'copied too')

  const recorder = startRecorder(t, root, {
    logFiles: [
      { name: 'runtime.log', path: runtimeLog },
      { name: 'app.log', path: appLog },
      { name: 'missing.log', path: missingLog },
    ],
    crashDir,
  })
  recorder.setDeviceInfo(DEVICE)
  recorder.event('apply', { path: 'x' })
  const sessionDir = recorder.status().sessionDir

  const result = recorder.archive('test')
  assert.equal(result.enabled, false)

  assert.equal(fs.existsSync(path.join(root, 'current.jsonl')), false)
  assert.ok(fs.existsSync(path.join(sessionDir, 'session.jsonl')))
  assert.ok(fs.existsSync(path.join(sessionDir, 'summary.json')))
  assert.ok(fs.existsSync(path.join(sessionDir, 'device.json')))
  assert.equal(fs.readFileSync(path.join(sessionDir, 'runtime.log'), 'utf8'), 'runtime line\n')
  assert.equal(fs.readFileSync(path.join(sessionDir, 'app.log'), 'utf8'), 'app line\n')
  assert.equal(fs.existsSync(path.join(sessionDir, 'missing.log')), false)
  assert.equal(fs.readFileSync(path.join(sessionDir, 'crashes', 'crash-1.dmp'), 'utf8'), 'dump-bytes')
  assert.equal(
    fs.readFileSync(path.join(sessionDir, 'crashes', 'crash-2.dmp'), 'utf8'),
    'dump-bytes-2',
  )
  assert.equal(fs.readFileSync(path.join(sessionDir, 'crashes', 'notes.txt'), 'utf8'), 'copied too')

  const summary = JSON.parse(fs.readFileSync(path.join(sessionDir, 'summary.json'), 'utf8'))
  assert.ok(summary.lastEvents.some((entry) => entry.event === 'apply'))
  assert.ok(summary.lastEvents.some((entry) => entry.event === 'archive'))

  const lines = readJsonLines(path.join(sessionDir, 'session.jsonl'))
  assert.equal(lines.at(-1).kind, 'event')
  assert.equal(lines.at(-1).event, 'archive')
  assert.deepEqual(lines.at(-1).data, { reason: 'test' })

  // Idempotent: a second archive must not move anything again.
  const size = fs.statSync(path.join(sessionDir, 'session.jsonl')).size
  assert.doesNotThrow(() => recorder.archive('again'))
  assert.equal(fs.statSync(path.join(sessionDir, 'session.jsonl')).size, size)
  assert.equal(fs.existsSync(path.join(root, 'current.jsonl')), false)
})

test('archive prunes session directories to keepSessions', (t) => {
  const root = tempRoot(t)
  const sessions = path.join(root, 'sessions')
  fs.mkdirSync(sessions, { recursive: true })

  const names = []
  for (let day = 1; day <= 12; day += 1) {
    const name = `2020-01-${String(day).padStart(2, '0')}T00-00-00-000Z`
    names.push(name)
    fs.mkdirSync(path.join(sessions, name), { recursive: true })
    fs.writeFileSync(path.join(sessions, name, 'session.jsonl'), '{}\n')
  }

  const recorder = makeRecorder(root, { keepSessions: 10 })
  t.after(() => recorder.stop())
  recorder.archive('test')

  const remaining = fs.readdirSync(sessions).sort()
  assert.equal(remaining.length, 10)
  assert.deepEqual(remaining, names.slice(2).sort())
  assert.equal(fs.existsSync(path.join(sessions, names[0])), false)
  assert.equal(fs.existsSync(path.join(sessions, names[1])), false)
})

test('start and archive never prune the current session when recovered dirs exist', (t) => {
  const root = tempRoot(t)
  const sessions = path.join(root, 'sessions')
  fs.mkdirSync(sessions, { recursive: true })

  // Newer than the fixed current session time, so a pure recency sort would
  // drop the current session unless it is explicitly protected.
  const recovered = []
  for (let i = 0; i < 10; i += 1) {
    const name = `recovered-2026-10-30T13-00-${String(i).padStart(2, '0')}-000Z`
    recovered.push(name)
    fs.mkdirSync(path.join(sessions, name), { recursive: true })
    fs.writeFileSync(path.join(sessions, name, 'session.jsonl'), '{}\n')
  }

  const recorder = startRecorder(t, root, { keepSessions: 10 })
  const currentDir = recorder.status().sessionDir

  // start() prunes immediately; the current session is protected and counts
  // against keepSessions.
  assert.ok(fs.existsSync(currentDir))
  assert.ok(fs.readdirSync(sessions).length <= 10)
  assert.equal(fs.existsSync(path.join(sessions, recovered[0])), false)

  recorder.archive('test')

  assert.ok(fs.existsSync(currentDir))
  assert.ok(fs.existsSync(path.join(currentDir, 'session.jsonl')))
  assert.ok(fs.readdirSync(sessions).length <= 10)
})

// ─── cap + crash recovery ────────────────────────────────────────────────────

test('maxBytes writes one cap-reached event and stops appending', async (t) => {
  const root = tempRoot(t)
  const recorder = startRecorder(t, root, { maxBytes: 1024 })

  await recorder.sampleNow()
  const currentFile = path.join(root, 'current.jsonl')
  const before = fs.statSync(currentFile).size

  assert.equal(recorder.event('big', { text: 'x'.repeat(4096) }), false)
  const after = fs.statSync(currentFile).size
  assert.ok(after > before)
  assert.equal(recorder.status().capReached, true)
  assert.equal(recorder.status().bytes, after)

  const lines = readJsonLines(currentFile)
  const caps = lines.filter((line) => line.kind === 'event' && line.event === 'cap-reached')
  assert.equal(caps.length, 1)
  assert.equal(typeof caps[0].data.bytes, 'number')
  assert.equal(recorder.writeSummary().capReached, true)

  assert.equal(recorder.event('after', {}), false)
  assert.equal(fs.statSync(currentFile).size, after)
  assert.equal(readJsonLines(currentFile).some((line) => line.event === 'after'), false)

  await recorder.sampleNow()
  assert.equal(fs.statSync(currentFile).size, after)
  assert.equal(
    readJsonLines(currentFile).filter((line) => line.kind === 'sample').length,
    1,
  )
})

test('start files a crashed current.jsonl back into the session that wrote it', (t) => {
  const root = tempRoot(t)
  let clock = FIXED_NOW

  const first = makeRecorder(root, { now: () => clock })
  t.after(() => first.stop())
  first.start()
  const crashedDir = first.status().sessionDir
  first.event('before-crash', { ok: true })
  first.stop() // a crash leaves current.jsonl in place; stop only clears timers

  clock += 5000

  const second = makeRecorder(root, { now: () => clock })
  t.after(() => second.stop())
  second.start()

  const recoveredLines = readJsonLines(path.join(crashedDir, 'session.jsonl'))
  assert.equal(recoveredLines[0].kind, 'session')
  assert.ok(recoveredLines.some((line) => line.event === 'before-crash'))
  assert.ok(fs.existsSync(path.join(crashedDir, 'summary.json')))

  const sessions = fs.readdirSync(path.join(root, 'sessions'))
  assert.equal(sessions.filter((name) => name.startsWith('recovered-')).length, 0)
  assert.ok(sessions.includes(path.basename(crashedDir)))

  const fresh = readJsonLines(path.join(root, 'current.jsonl'))
  assert.equal(fresh[0].kind, 'session')
  assert.notEqual(fresh[0].session, path.basename(crashedDir))
  assert.ok(fs.existsSync(path.join(root, 'sessions', fresh[0].session)))
})

test('start removes stale empty session directories', (t) => {
  const root = tempRoot(t)
  const sessions = path.join(root, 'sessions')
  fs.mkdirSync(path.join(sessions, '2020-01-01T00-00-00-000Z'), { recursive: true })
  fs.mkdirSync(path.join(sessions, 'recovered-2020-01-02T00-00-00-000Z'), { recursive: true })
  fs.mkdirSync(path.join(sessions, '2020-01-03T00-00-00-000Z'), { recursive: true })
  fs.writeFileSync(
    path.join(root, 'current.jsonl'),
    `${JSON.stringify({ kind: 'session', session: '2020-01-03T00-00-00-000Z' })}\n`,
  )

  const recorder = startRecorder(t, root)
  const names = fs.readdirSync(sessions)

  assert.equal(names.includes('2020-01-01T00-00-00-000Z'), false)
  assert.equal(names.includes('recovered-2020-01-02T00-00-00-000Z'), false)
  assert.ok(names.includes('2020-01-03T00-00-00-000Z'))
  assert.ok(fs.existsSync(path.join(sessions, '2020-01-03T00-00-00-000Z', 'session.jsonl')))
  assert.equal(recorder.status().enabled, true)
})

test('start falls back to recovered-* when current.jsonl has no session record', (t) => {
  const root = tempRoot(t)
  const sessions = path.join(root, 'sessions')
  fs.mkdirSync(sessions, { recursive: true })
  fs.writeFileSync(path.join(root, 'current.jsonl'), 'not json\n')

  const recorder = startRecorder(t, root)
  const recovered = fs.readdirSync(sessions).filter((name) => name.startsWith('recovered-'))

  assert.equal(recovered.length, 1)
  assert.ok(fs.existsSync(path.join(sessions, recovered[0], 'session.jsonl')))
  assert.equal(recorder.status().enabled, true)
})

// ─── status ──────────────────────────────────────────────────────────────────

test('status reports the documented fields before and after start', async (t) => {
  const root = tempRoot(t)
  const recorder = makeRecorder(root, { fallback: true })
  t.after(() => recorder.stop())

  const idle = recorder.status()
  assert.deepEqual(Object.keys(idle).sort(), [
    'bytes',
    'capReached',
    'dir',
    'enabled',
    'events',
    'fallback',
    'lastSampleAt',
    'samples',
    'sessionDir',
  ])
  assert.equal(idle.enabled, false)
  assert.equal(idle.dir, root)
  assert.equal(idle.sessionDir, null)
  assert.equal(idle.bytes, 0)
  assert.equal(idle.samples, 0)
  assert.equal(idle.events, 0)
  assert.equal(idle.lastSampleAt, null)
  assert.equal(idle.capReached, false)
  assert.equal(idle.fallback, true)

  recorder.start()
  await recorder.sampleNow()
  assert.equal(recorder.event('setting', { keyPath: 'x', value: 1 }), true)

  const active = recorder.status()
  assert.equal(active.enabled, true)
  assert.equal(active.dir, root)
  assert.equal(path.dirname(active.sessionDir), path.join(root, 'sessions'))
  assert.ok(fs.existsSync(active.sessionDir))
  assert.ok(active.bytes > 0)
  assert.equal(active.samples, 1)
  assert.equal(active.events, 1)
  assert.match(active.lastSampleAt, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/)
  assert.equal(active.capReached, false)
  assert.equal(active.fallback, true)
})
