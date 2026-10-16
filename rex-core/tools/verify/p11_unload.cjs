// P1-1 headless: video -> UNLOAD -> image transition runs clean.
// Asserts the engine path (UnloadScene disarms recovery, state returns to
// non-Running, no crash, telemetry sane across a 10 s watch). WorkerW
// visibility itself (SetVisible(false) hiding the layer) is NOT observable
// headless — recorded as the interactive remainder.
// Usage: node p11_unload.cjs <runtime> <video> <image>
const { spawn } = require('node:child_process')

const RT = process.argv[2]
const VIDEO = process.argv[3]
const IMAGE = process.argv[4]

const proc = spawn(RT, ['--runtime'], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true })
let buf = Buffer.alloc(0)
let nextId = 1
const pending = new Map()
let exited = null
const events = []
proc.stdout.on('data', (c) => {
  buf = Buffer.concat([buf, c])
  while (buf.length >= 4) {
    const len = buf.readUInt32LE(0)
    if (buf.length < 4 + len) return
    const msg = JSON.parse(buf.subarray(4, 4 + len).toString('utf8'))
    buf = buf.subarray(4 + len)
    if (msg.type === 'response') {
      const p = pending.get(msg.id); pending.delete(msg.id)
      if (p) p(msg)
    } else if (msg.type === 'event') { events.push(msg.event) }
  }
})
proc.stderr.on('data', () => {})
proc.on('exit', (c) => { exited = c })

function send(cmd, params = {}, timeoutMs = 15000) {
  return new Promise((resolve) => {
    const id = nextId++
    const payload = JSON.stringify({ id, cmd, params })
    const frame = Buffer.allocUnsafe(4 + Buffer.byteLength(payload))
    frame.writeUInt32LE(Buffer.byteLength(payload), 0)
    frame.write(payload, 4, 'utf8')
    const t = setTimeout(() => { pending.delete(id); resolve({ ok: false, error: 'timeout' }) }, timeoutMs)
    pending.set(id, (m) => { clearTimeout(t); resolve(m) })
    proc.stdin.write(frame)
  })
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms))
let failures = 0
const check = (n, c, x) => {
  console.log((c ? 'ok: ' : 'FAIL: ') + n + (x !== undefined && !c ? ` [${x}]` : ''))
  if (!c) failures++
}

;(async () => {
  check('INIT', (await send('INIT', { maxFps: 0, hardwareAcceleration: true, protocolVersion: 1, pauseOnFullscreen: false })).ok)
  check('LOAD video', (await send('LOAD', { path: VIDEO })).ok)
  check('PLAY video', (await send('PLAY', {})).ok)
  await wait(10000)
  if (exited !== null) { console.log('FAIL: runtime died during video playback'); process.exit(2) }
  const st1 = await send('GET_STATUS', {}, 8000)
  check('video playing (effFps>0)', Number(st1?.data?.telemetry?.effectiveVideoFps ?? st1?.data?.effectiveVideoFps ?? 0) > 0)
  check('UNLOAD ok', (await send('UNLOAD', {})).ok)
  check('UNLOADED event', events.includes('UNLOADED'))
  await wait(2000)
  check('alive after UNLOAD', exited === null)
  const st2 = await send('GET_STATUS', {}, 8000)
  check('status sane after UNLOAD', !!st2?.data)
  check('LOAD image', (await send('LOAD', { path: IMAGE, type: 'image' })).ok)
  await wait(3000)
  check('alive after image LOAD', exited === null)
  check('STOP ok', (await send('STOP', {})).ok)
  await send('SHUTDOWN', {})
  await wait(1500)
  console.log(failures === 0 ? 'ALL CHECKS PASSED' : `${failures} CHECKS FAILED`)
  try { proc.kill() } catch {}
  process.exit(failures === 0 ? 0 : 1)
})().catch((e) => { console.error('HARNESS ERROR:', e); try { proc.kill() } catch {}; process.exit(2) })
