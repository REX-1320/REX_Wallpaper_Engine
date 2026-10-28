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
    imageDwellMs: opts.imageDwellMs,
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

test('a scene-end mode gives a still image a dwell before advancing', async () => {
  const h = harness()

  // The desktop holds an image, which emits no ENDED: without a dwell the
  // rotation parks here forever (TODO P2, found live 2026-10-24).
  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path }))

  assert.deepEqual(h.dueIn(), [30_000])
  assert.equal(h.applied.length, 0)

  h.advance(30_000)
  await h.runDue()

  assert.equal(h.applied[0].path, A.path)
  assert.equal(h.events.at(-1).reason, 'After Current Ends')
  // The next member is a video again, so the clock is the video's own end.
  assert.deepEqual(h.dueIn(), [])
})

test('rebroadcasting a scene-end plan does not restart the image dwell', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path }))
  h.advance(10_000)
  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path, updatedAt: h.now() + 1 }))

  assert.deepEqual(h.dueIn(), [20_000])
})

test('landing on an image inside a scene-end mode arms the dwell', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ mode: 'Video Ends', currentPath: A.path, queue: [A, C, B] }))
  assert.deepEqual(h.dueIn(), [], 'a video scene is event-driven, not timed')

  await h.scheduler.handleEvent('ENDED', { path: A.path })
  assert.equal(h.applied[0].path, C.path)
  assert.deepEqual(h.dueIn(), [30_000])

  h.advance(30_000)
  await h.runDue()
  assert.equal(h.applied[1].path, B.path)
  assert.deepEqual(h.dueIn(), [])
})

test('the image dwell is configurable and never applies to the Timer', async () => {
  const h = harness({ imageDwellMs: 5_000 })

  h.scheduler.setPlan(makePlan({ mode: 'After Current Ends', currentPath: C.path }))
  assert.deepEqual(h.dueIn(), [5_000])

  h.scheduler.setPlan(makePlan({ mode: 'Timer', currentPath: C.path, updatedAt: h.now() + 1 }))
  assert.deepEqual(h.dueIn(), [60_000], 'the Timer owns the clock, image or not')
})

// Regression (live 2026-10-28): clicking "Apply playlist" while that
// playlist's persisted Timer was already overdue made main advance to the
// member AFTER the one just applied. The plan's remainingMs belonged to a
// previous session; a new playlist intent must start its own interval.
test('a different playlist starts a fresh interval even with an overdue timer', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ playlistId: 'pl-1', currentPath: A.path }))

  h.scheduler.setPlan(makePlan({
    playlistId: 'pl-2',
    playlistName: 'Nature',
    currentPath: C.path,
    queue: [C, B],
    updatedAt: h.now() + 1,
    timer: {
      ...makePlan().timer,
      remainingMs: 0,
      startedAt: h.now() - 600_000,
      syncedAt: h.now() - 600_000,
    },
  }))

  assert.deepEqual(h.dueIn(), [60_000], 'the first member gets its full interval')
  assert.equal(h.applied.length, 0)

  h.advance(60_000)
  await h.runDue()
  assert.equal(h.applied[0].path, B.path)
})

// The same rule for a user applying another member of the playlist that is
// already running: the interval restarts at the apply, it does not inherit the
// deadline that was about to fire.
test('a user apply inside the same playlist restarts the timer', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ currentPath: A.path }))
  h.advance(50_000)

  h.scheduler.setPlan(makePlan({
    currentPath: B.path,
    updatedAt: h.now() + 1,
    timer: { ...makePlan().timer, remainingMs: 10_000, syncedAt: h.now() },
  }))

  assert.deepEqual(h.dueIn(), [60_000])
})

// Regression (live 2026-10-28): the renderer's 500 ms tick built a plan a few
// ms after main applied the next member, but before the applied event was
// adopted. That plan still named the PREVIOUS file; re-anchoring to it made
// the cursor jump backwards and the timer fire a second time.
test('a stale post-apply plan does not drag the cursor back to the previous file', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ currentPath: A.path }))
  h.advance(60_000)
  await h.runDue()

  assert.equal(h.applied.length, 1)
  assert.equal(h.scheduler.state().currentPath, B.path)

  h.advance(1)
  h.scheduler.setPlan(makePlan({
    currentPath: A.path,
    updatedAt: h.now(),
    timer: { ...makePlan().timer, remainingMs: 0, syncedAt: h.now() },
  }))

  assert.equal(h.scheduler.state().currentPath, B.path, 'the applied file stays the cursor')
  assert.deepEqual(h.dueIn(), [59_999])

  h.advance(59_999)
  await h.runDue()
  assert.deepEqual(h.applied.map((entry) => entry.path), [B.path, C.path])
})

// A gallery apply mirrors a null plan. That is a stop, not "keep the old
// rotation until the next plan arrives": an overdue Timer must not fire over
// the file the user just put on the desktop.
test('clearing the plan stops a running timer immediately', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan())
  h.scheduler.setPlan(null)

  assert.equal(h.scheduler.state().active, false)
  assert.equal(h.scheduler.state().playlistId, null)
  assert.deepEqual(h.dueIn(), [])

  h.advance(120_000)
  await h.runDue()
  assert.equal(h.applied.length, 0)
})

// A plan with no queue cannot govern anything; it must never schedule a
// phantom apply.
test('a plan with no queue is inert', async () => {
  const h = harness()

  h.scheduler.setPlan(makePlan({ queue: [] }))
  assert.equal(h.scheduler.state().active, false)
  assert.deepEqual(h.dueIn(), [])

  h.advance(120_000)
  await h.runDue()
  assert.equal(h.applied.length, 0)
})

// Regression (live 2026-10-28 15:55:48): the Timer started a native apply and
// the user applied another file while it was still loading. The old apply
// finished afterwards and re-armed its own timer / overwrote the cursor of the
// plan that had replaced it. The native call cannot be cancelled, but its
// result must be discarded.
test('a plan replaced mid-apply discards the stale result', async () => {
  let release
  const gate = new Promise((resolve) => {
    release = resolve
  })
  const h = harness({
    apply: async (entry) => {
      if (entry.path === B.path) await gate
      return { ok: true }
    },
  })

  h.scheduler.setPlan(makePlan({ currentPath: A.path }))
  h.advance(60_000)
  const running = h.runDue()

  // The user applies a gallery file while the Timer's native apply is loading.
  h.scheduler.setPlan(null)
  release()
  await running

  assert.equal(h.applied.length, 1, 'the native apply happened')
  assert.equal(h.events.length, 0, 'no rotation is reported for the replaced plan')
  assert.equal(h.scheduler.state().active, false)
  assert.equal(h.scheduler.state().playlistId, null)
  assert.equal(h.scheduler.state().currentPath, null)
  assert.deepEqual(h.dueIn(), [])
})

// Same race inside one playlist: the user applies C from the strip while the
// Timer is still loading B. C must stay the cursor and get the full interval.
test('a user apply during a timer apply wins over the in-flight member', async () => {
  let release
  const gate = new Promise((resolve) => {
    release = resolve
  })
  const h = harness({
    apply: async (entry) => {
      if (entry.path === B.path) await gate
      return { ok: true }
    },
  })

  h.scheduler.setPlan(makePlan({ currentPath: A.path }))
  h.advance(60_000)
  const running = h.runDue()

  h.scheduler.setPlan(makePlan({
    currentPath: C.path,
    updatedAt: h.now() + 1,
    timer: { ...makePlan().timer, remainingMs: 60_000, startedAt: h.now(), syncedAt: h.now() },
  }))
  release()
  await running

  assert.equal(h.scheduler.state().currentPath, C.path)
  assert.equal(h.events.length, 0)
  assert.deepEqual(h.dueIn(), [60_000])
})
