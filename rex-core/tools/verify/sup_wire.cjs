'use strict'
// P0.2 headless proof: REAL WallpaperRuntime + RuntimeManager + Supervisor +
// CrashReporter + StateStore against a fake native. Scenarios A-G.
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { WallpaperRuntime } = require('D:\\REX_doc\\app\\electron\\wallpaperRuntime.cjs')
const { RuntimeManager } = require('D:\\REX_doc\\app\\electron\\supervisor\\runtime-manager.cjs')
const { Supervisor } = require('D:\\REX_doc\\app\\electron\\supervisor\\supervisor.cjs')
const { CrashReporter } = require('D:\\REX_doc\\app\\electron\\supervisor\\crash-reporter.cjs')
const { StateStore } = require('D:\\REX_doc\\app\\electron\\supervisor\\state-store.cjs')

const FAKE = 'C:\\Users\\Moinu\\AppData\\Local\\Temp\\opencode\\rex-verify\\fake_native.cjs'
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
let failures = 0
function check(name, cond, extra) {
  console.log((cond ? 'PASS ' : 'FAIL ') + name + (extra ? ' [' + extra + ']' : ''))
  if (!cond) failures++
}
function tmpDir(tag) {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'sup-' + tag + '-'))
  return d
}
function makeEnv(tag, opts, fakeEnv) {
  const dir = tmpDir(tag)
  const log = []
  const events = []
  const avail = []
  const env = {
    dir, log, events, avail,
    reapplies: 0,
    reapplyResult: true,
    win: null,
  }
  const rt = new WallpaperRuntime('node', (...a) => log.push(a.join(' ')))
  rt.cliPathFake = true
  // Point the client at the fake native instead of spawning node bare:
  const origSpawn = rt._spawn.bind(rt)
  rt._spawn = () => {
    const { spawn } = require('node:child_process')
    const proc = spawn(process.execPath, [FAKE], { stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, ...(fakeEnv || {}) } })
    rt.proc = proc
    rt.buffer = Buffer.alloc(0)
    proc.stdout.on('data', (chunk) => rt._onData(chunk))
    proc.stderr.on('data', () => {})
    proc.on('error', (err) => { rt.logLine('wallpaper-runtime error:', err.message); rt._teardown(proc, 'spawn failed') })
    proc.on('exit', (code) => {
      rt.logLine('wallpaper-runtime exited with code', code)
      if (rt.proc === proc) rt._teardown(proc, 'runtime exited (' + code + ')')
      rt.emit('exit', code)
    })
    return proc
  }
  const deps = {
    runtime: rt,
    logLine: (...a) => log.push(a.join(' ')),
    getWindow: () => env.win,
    getLastApplied: () => ({ kind: 'video', params: { path: 'x.mp4' } }),
    reapply: async () => { env.reapplies++; return env.reapplyResult },
    setRuntimeAvailable: (v) => avail.push(v),
    onEvent: (m) => events.push(m.event),
    stateFilePath: path.join(dir, 'supervisor-state.json'),
    crashReporter: new CrashReporter({ baseDir: path.join(dir, 'crashes') }),
    minidump: { capture: async () => ({ ok: false }) },
    logs: {},
    options: opts || {},
  }
  const sup = new Supervisor(deps)
  return { env, rt, sup }
}

async function scenarioA() {
  // exit(2) with working reapply -> recovery-ok, counter reset, available(true), exactly 1 reapply
  const { env, rt, sup } = makeEnv('a', {}, { FAKE_EXIT_AFTER_MS: '1200', FAKE_EXIT_CODE: '2' })
  await rt.init(60, true)
  sup.start()
  await sleep(2500)
  const st = sup.getStatus()
  check('A: exactly 1 reapply', env.reapplies === 1, 'got=' + env.reapplies)
  check('A: attempts reset', st.recoveryAttempts === 0, 'got=' + st.recoveryAttempts)
  check('A: available(true) last', env.avail[env.avail.length - 1] === true)
  check('A: recovery-ok event', env.events.includes('recovery-ok'))
  sup.stop(); try { rt.proc && rt.proc.kill() } catch {}
}

async function scenarioB() {
  // 4 exits with failing reapply -> 3 reapplies then park + gave-up
  const { env, rt, sup } = makeEnv('b', {}, { FAKE_EXIT_AFTER_MS: '400', FAKE_EXIT_CODE: '2' })
  env.reapplyResult = false
  // keep killing: re-spawn fake on each recovery by re-init inside reapply
  const { spawn } = require('node:child_process')
  env.reapply = null
  sup.deps.reapply = async () => {
    env.reapplies++
    await rt.init(60, true)
    await sleep(100)
    try { rt.proc && rt.proc.kill() } catch {}
    return false
  }
  await rt.init(60, true)
  sup.start()
  await sleep(4500)
  check('B: exactly 3 reapplies then park', env.reapplies === 3, 'got=' + env.reapplies)
  check('B: gave-up event', env.events.includes('recovery-gave-up'))
  sup.stop(); try { rt.proc && rt.proc.kill() } catch {}
}

async function scenarioC() {
  // stop() then exit -> ignored
  const { env, rt, sup } = makeEnv('c', {}, {})
  await rt.init(60, true)
  sup.start()
  sup.stop()
  try { rt.proc && rt.proc.kill() } catch {}
  await sleep(600)
  check('C: no reapply after stop', env.reapplies === 0)
  check('C: no recovery events', env.events.length === 0, JSON.stringify(env.events))
}

async function scenarioD() {
  // renderer guard: visible window, NEVER any ping -> no reload
  const { env, rt, sup } = makeEnv('d', { rendererCheckIntervalMs: 100, rendererStaleMs: 300 })
  let reloads = 0
  env.win = { isDestroyed: () => false, isVisible: () => true, webContents: { isCrashed: () => false, reload: () => { reloads++ } } }
  sup.start()
  await sleep(1200)
  check('D: no reload without any ping (guard)', reloads === 0, 'reloads=' + reloads)
  sup.stop()
}

async function scenarioE() {
  // watchdog armed: one ping, then stale -> exactly >=1 reload
  const { env, rt, sup } = makeEnv('e', { rendererCheckIntervalMs: 100, rendererStaleMs: 300 })
  let reloads = 0
  env.win = { isDestroyed: () => false, isVisible: () => true, webContents: { isCrashed: () => false, reload: () => { reloads++ } } }
  sup.start()
  sup.recordRendererPing()
  await sleep(1200)
  check('E: reload fires once stale after a ping', reloads >= 1, 'reloads=' + reloads)
  sup.stop()
}

async function scenarioF() {
  // hung path ACTIVE (missLimit 2, fast heartbeat, PING ignored) -> stop() kills fake -> exit -> recovery
  const { env, rt, sup } = makeEnv('f', { heartbeatIntervalMs: 100, pingTimeoutMs: 150, heartbeatMissLimit: 2 }, {})
  await rt.init(60, true)
  sup.start()
  await sleep(2500)
  const st = sup.getStatus()
  check('F: hung runtime stopped+recovered', env.reapplies >= 1, 'reapplies=' + env.reapplies)
  sup.stop(); try { rt.proc && rt.proc.kill() } catch {}
}

async function scenarioG() {
  // parked heartbeat (main wiring: Infinity) with PING ignored, runtime alive -> untouched
  const { env, rt, sup } = makeEnv('g', { heartbeatIntervalMs: 100, pingTimeoutMs: 150, heartbeatMissLimit: Number.POSITIVE_INFINITY }, {})
  await rt.init(60, true)
  sup.start()
  await sleep(1200)
  const st = sup.getStatus()
  check('G: parked heartbeat never kills healthy runtime', env.reapplies === 0 && st.runtimeAlive === true)
  sup.stop(); try { rt.proc && rt.proc.kill() } catch {}
}

async function main() {
  await scenarioA()
  await scenarioB()
  await scenarioC()
  await scenarioD()
  await scenarioE()
  await scenarioF()
  await scenarioG()
  console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURES')
  process.exit(failures === 0 ? 0 : 1)
}
main()
