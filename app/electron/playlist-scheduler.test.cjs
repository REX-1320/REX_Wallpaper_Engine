const test = require('node:test')
const assert = require('node:assert/strict')
const { PlaylistScheduler } = require('./playlist-scheduler.cjs')

const A = { path: 'C:\\wallpapers\\a.mp4', apply: { kind: 'video', mode: 'fill' } }
const B = { path: 'C:\\wallpapers\\b.mp4', apply: { kind: 'video', mode: 'fill' } }
const C = { path: 'C:\\wallpapers\\c.png', apply: { kind: 'image', mode: 'fill' } }

function makePlan(overrides = {}) {
  return {
    version: 1,
    playlistId: 'pl-1',
    playlistName: 'Favorites',
    mode: 'Timer',
    currentPath: A.path,
    loop: true,
    paused: false,
    updatedAt: 1_000_000,
    queue: [A, B, C],
    timer: {
      totalMs: 60_000,
      remainingMs: 60_000,
      running: true,
      startedAt: 1_000_000,
      accumulatedMs: 0,
      syncedAt: 1_000_000,
    },
    ...overrides,
  }
}

function harness(opts = {}) {
  let now = 1_000_000
  let nextHandle = 1
  const timers = new Map()
  const applied = []
  const events = []
  const persisted = []

  const scheduler = new PlaylistScheduler({
    now: () => now,
    setTimer: (fn, ms) => {
      const handle = nextHandle++
      timers.set(handle, { fn, due: now + ms })
      return handle
    },
    clearTimer: (handle) => timers.delete(handle),
    apply: async (entry) => {
      applied.push(entry)
      if (opts.apply) return opts.apply(entry)
      return { ok: true }
    },
    exists: (entry) => (opts.exists ? opts.exists(entry) : true),
    emit: (event) => events.push(event),
    persist: (snapshot) => persisted.push(snapshot),
    log: () => {},
    retryMs: opts.retryMs ?? 60_000,
  })

  const dueIn = () => [...timers.values()].map((timer) => timer.due - now).sort((a, b) => a - b)

  async function runDue() {
    let ran = false
    do {
      ran = false
      for (const [handle, timer] of [...timers]) {
        if (timer.due > now) continue
        timers.delete(handle)
        ran = true
        await timer.fn()
      }
    } while (ran)
  }

  return {
    scheduler,
    applied,
    events,
    persisted,
    dueIn,
    runDue,
    advance: (ms) => {
      now += ms
    },
    now: () => now,
  }
}

test('Timer advances to the next member when it elapses', async () => {
  const h = harness()
  h.scheduler.setPlan(makePlan())

  assert.deepEqual(h.dueIn(), [60_000])

  h.advance(60_000)
  await h.runDue()

  assert.equal(h.applied.length, 1)
  assert.equal(h.applied[0].path, B.path)
  assert.equal(h.events.at(-1).path, B.path)
  assert.equal(h.events.at(-1).reason, 'Timer elapsed')
  assert.equal(h.events.at(-1).nextDueAt, h.now() + 60_000)
  assert.deepEqual(h.dueIn(), [60_000])
})

test('rebroadcasting the same timer does not restart its countdown', async () => {
  const h = harness()
  h.scheduler.setPlan(makePlan())

  h.advance(30_000)
  h.scheduler.setPlan(makePlan({
    updatedAt: h.now(),
    timer: { ...makePlan().timer, remainingMs: 30_000, syncedAt: h.now() },
  }))

  assert.deepEqual(h.dueIn(), [30_000])

  h.advance(30_000)
  await h.runDue()

  assert.equal(h.applied[0].path, B.path)
})

test('editing the timer re-arms the countdown from the edited value', async () => {
  const h = harness()
  h.scheduler.setPlan(makePlan())

  h.advance(30_000)
  h.scheduler.setPlan(makePlan({
    updatedAt: h.now(),
    timer: {
      totalMs: 120_000,
      remainingMs: 120_000,
      running: true,
      startedAt: h.now(),
      accumulatedMs: 0,
      syncedAt: h.now(),
    },
  }))

  assert.deepEqual(h.dueIn(), [120_000])
})

test('a scene-end event advances only the scene-end modes and only the live scene', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'Manual' }))
  await h.scheduler.handleEvent('ENDED', { path: A.path })
  assert.equal(h.applied.length, 0)

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends' }))
  await h.scheduler.handleEvent('ENDED', { path: B.path })
  assert.equal(h.applied.length, 0)

  await h.scheduler.handleEvent('ENDED', { path: A.path })
  assert.equal(h.applied.length, 1)
  assert.equal(h.applied[0].path, B.path)
  assert.equal(h.events.at(-1).reason, 'After Current Ends')
})

test('a one-run playlist stops after its last member', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: A.path, loop: false }))
  await h.scheduler.handleEvent('ENDED', { path: A.path })
  assert.equal(h.applied[0].path, B.path)

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path, loop: false, updatedAt: h.now() + 1 }))
  await h.scheduler.handleEvent('ENDED', { path: C.path })

  assert.equal(h.applied.length, 1)
  assert.equal(h.scheduler.state().active, false)
})

test('a looping playlist wraps back to its first member', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'Video Ends', currentPath: C.path, loop: true }))
  await h.scheduler.handleEvent('ENDED', { path: C.path })

  assert.equal(h.applied[0].path, A.path)
})

test('a missing or failing member is skipped', async () => {
  const h = harness({
    apply: async (entry) => {
      if (entry.path === B.path) throw new Error('missing file')
      return { ok: true }
    },
  })

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends' }))
  await h.scheduler.handleEvent('ENDED', { path: A.path })

  assert.deepEqual(h.applied.map((entry) => entry.path), [B.path, C.path])
  assert.equal(h.scheduler.state().currentPath, C.path)
  assert.equal(h.scheduler.state().active, true)
})

test('a paused timer schedules nothing and a paused scene-end mode ignores ENDED', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({
    timer: { ...makePlan().timer, running: false },
  }))
  assert.deepEqual(h.dueIn(), [])
  assert.equal(h.scheduler.state().active, false)

  h.scheduler.setPlan(makePlan({ mode: 'Video Ends', paused: true, updatedAt: h.now() + 1 }))
  await h.scheduler.handleEvent('ENDED', { path: A.path })
  assert.equal(h.applied.length, 0)
})

test('a restored overdue timer fires immediately', async () => {
  const h = harness()

  h.scheduler.restore({ plan: makePlan(), currentPath: A.path, nextDueAt: h.now() - 5_000 })

  assert.deepEqual(h.dueIn(), [0])
  await h.runDue()
  assert.equal(h.applied[0].path, B.path)
})

test('a current file outside the queue starts the rotation at its first member', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: 'C:\\wallpapers\\other.mp4' }))
  await h.scheduler.handleEvent('ENDED', { path: 'C:\\wallpapers\\other.mp4' })

  assert.equal(h.applied[0].path, A.path)
})

test('a newer plan re-anchors the cursor to the file on the desktop', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends' }))
  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: B.path, updatedAt: h.now() + 5_000 }))

  await h.scheduler.handleEvent('ENDED', { path: B.path })
  assert.equal(h.applied[0].path, C.path)
})

test('a stale plan does not re-fire the timer just after a main-process rotation', async () => {
  const h = harness()
  const first = makePlan()

  h.scheduler.setPlan(first)
  h.advance(60_000)
  await h.runDue()
  assert.deepEqual(h.dueIn(), [60_000])

  // The renderer had not processed the applied event yet and rebroadcasts the
  // pre-rotation plan. Its timer is already past; it must not fire a second time.
  h.scheduler.setPlan({ ...first, updatedAt: first.updatedAt })

  assert.deepEqual(h.dueIn(), [60_000])
  assert.equal(h.applied.length, 1)
})

test('a plan with no next member stops instead of scheduling a phantom change', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path, loop: false }))
  await h.scheduler.handleEvent('ENDED', { path: C.path })

  assert.deepEqual(h.dueIn(), [])
  assert.equal(h.scheduler.state().active, false)
  assert.equal(h.applied.length, 0)
})
