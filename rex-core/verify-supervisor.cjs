// Smoke test for the supervisor pieces against the real built binaries:
//   1. PING heartbeat against a live rex-wallpaper-runtime.exe
//   2. Crash report writing
//   3. rex-minidump.exe capture of the live runtime process
//   4. Supervisor circuit breaker (runtime killed -> recovery -> report)
//
// Run: node verify-supervisor.js

const path = require('node:path')
const fs = require('node:fs')
const os = require('node:os')

const ROOT = path.resolve(__dirname, '..', 'app', 'electron')
const RUNTIME = path.resolve(__dirname, '..', 'rex-core', '.build', 'Release', 'rex-wallpaper-runtime.exe')
const { WallpaperRuntime } = require(path.join(ROOT, 'wallpaperRuntime.cjs'))
const { RuntimeManager } = require(path.join(ROOT, 'supervisor', 'runtime-manager.cjs'))
const { CrashReporter } = require(path.join(ROOT, 'supervisor', 'crash-reporter.cjs'))
const { Minidump } = require(path.join(ROOT, 'supervisor', 'minidump.cjs'))
const { Supervisor } = require(path.join(ROOT, 'supervisor', 'supervisor.cjs'))

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'rex-sup-'))
const log = (...a) => console.log(...a)
let failures = 0
const check = (name, cond, extra) => {
  if (cond) console.log('PASS', name)
  else { console.log('FAIL', name, extra ?? ''); failures++ }
}

async function main() {
  // 1. Runtime + PING. send() resolves msg.data (null for PING) on
  // success and rejects on timeout — so resolution itself is the proof of
  // life. (The old assertion `pong.pong === true` could never pass: native
  // sends null data.)
  const runtime = new WallpaperRuntime(RUNTIME, log)
  const mgr = new RuntimeManager(runtime)
  check('manager wraps runtime', mgr.runtime === runtime)
  await runtime.init(60, false)
  let pongOk = false
  try {
    await runtime.send('PING')
    pongOk = true
  } catch {}
  check('PING responds (resolves without timeout)', pongOk)
  check('alive', mgr.alive === true)
  const pingOk = await mgr.ping(2000)
  check('mgr.ping() true', pingOk === true)

  // 2. CrashReporter
  const reporter = new CrashReporter({ baseDir: path.join(tmp, 'crashes'), logLine: log })
  const reportDir = reporter.writeReport({
    reason: 'runtime-exit',
    exitCode: -1,
    pid: 9999,
    state: { lastWallpaper: 'C:/w/x.jpg', wallpaperType: 'image', fillMode: 'fill' },
    logs: { appLog: __filename },
  })
  check('report dir written', !!reportDir && fs.existsSync(path.join(reportDir, 'report.json')))
  check('system-info written', fs.existsSync(path.join(reportDir, 'system-info.json')))
  check('log copied', fs.existsSync(path.join(reportDir, 'rex-app.log')))

  // 3. Minidump of the live runtime
  const minidump = new Minidump({ logLine: log })
  const dmpPath = path.join(reporter.baseDir, `runtime-${runtime.proc.pid}.dmp`)
  const res = await minidump.capture(runtime.proc.pid, dmpPath)
  check('minidump captured', res.ok && fs.existsSync(dmpPath) && fs.statSync(dmpPath).size > 100000,
    JSON.stringify(res).slice(0, 200))

  // 4. Supervisor: kill runtime -> exit handler -> recovery attempt (reapply
  //    fails because reapply callback returns false here; circuit breaker).
  let reapplied = 0
  let availability = []
  const sup = new Supervisor({
    runtime,
    crashReporter: reporter,
    minidump,
    logLine: log,
    options: { heartbeatIntervalMs: 500, rendererCheckIntervalMs: 10000 },
    getWindow: () => null,
    getLastApplied: () => ({ kind: 'image', params: { path: 'C:/w/x.jpg', mode: 'fill' } }),
    reapply: async () => { reapplied++; return false },
    setRuntimeAvailable: (v) => availability.push(v),
    onEvent: () => {},
  })
  sup.start()
  // First tick probes PING (interval 500ms) and arms the heartbeat; poll
  // briefly instead of assuming a tick already ran.
  let armed = false
  for (let i = 0; i < 10 && !armed; i++) {
    await new Promise((r) => setTimeout(r, 200))
    armed = sup.getStatus().heartbeatArmed === true
  }
  check('heartbeat armed after PING probe', armed, JSON.stringify(sup.getStatus()))
  const pidBefore = runtime.proc.pid
  runtime.proc.kill() // simulate crash
  await new Promise((r) => setTimeout(r, 1500))
  sup.stop()
  check('exit triggered recovery', reapplied === 1, `reapplied=${reapplied}`)
  check('availability flagged false', availability.includes(false))
  const st = sup.getStatus()
  check('breaker state after failed recovery', st.recoveryAttempts === 1 && st.healthy === false, JSON.stringify(st))

  runtime.stop()
  console.log(failures === 0 ? '\nALL TESTS PASSED' : `\n${failures} FAILURES`)
  process.exit(failures === 0 ? 0 : 1)
}

main().catch((e) => { console.error('TEST CRASH', e); process.exit(2) })
