'use strict'
// 3-min per-second fps log: title | video->decoder->render->output fps.
// Usage: node fps_log.cjs <exe> <media> <title> <cap> <outfile>
const fs = require('node:fs')
const { spawn } = require('node:child_process')
const [exe, media, title, capArg, outFile] = process.argv.slice(2)
const cap = parseInt(capArg || '0', 10)
const N = parseInt(process.argv[7] || '180', 10)
const proc = spawn(exe, ['--runtime'], { stdio: ['pipe', 'pipe', 'pipe'] })
let buf = Buffer.alloc(0)
let nextId = 1
const pending = new Map()
proc.stdout.on('data', (chunk) => {
  buf = Buffer.concat([buf, chunk])
  while (buf.length >= 4) {
    const len = buf.readUInt32LE(0)
    if (buf.length < 4 + len) return
    const payload = buf.subarray(4, 4 + len).toString('utf8')
    buf = buf.subarray(4 + len)
    let msg
    try { msg = JSON.parse(payload) } catch { continue }
    if (msg.type === 'response') {
      const e = pending.get(msg.id)
      if (!e) continue
      pending.delete(msg.id)
      e(msg)
    }
  }
})
proc.stderr.on('data', () => {})
proc.on('exit', (code) => { try { fs.appendFileSync(outFile, 'RUNTIME_EXITED code=' + code + '\n') } catch (e) {} process.exit(2) })
proc.on('error', (e) => { try { fs.appendFileSync(outFile, 'SPAWN_ERROR ' + e.message + '\n') } catch (x) {} process.exit(2) })
proc.on('exit', (code) => { fs.appendFileSync(outFile, 'RUNTIME_EXITED code=' + code + '\n'); process.exit(2) })
function send(cmd, params) {
  const id = nextId++
  const payload = Buffer.from(JSON.stringify({ id, cmd, params: params || {} }), 'utf8')
  const frame = Buffer.allocUnsafe(4 + payload.length)
  frame.writeUInt32LE(payload.length, 0)
  payload.copy(frame, 4)
  return new Promise((resolve) => { pending.set(id, resolve); proc.stdin.write(frame) })
}
const f1 = (v) => Number(Number(v || 0).toFixed(1))
async function main() {
  fs.writeFileSync(outFile, 'PID=' + proc.pid + '\n')
  const init = await send('INIT', { maxFps: cap, hardwareAcceleration: true, pauseOnFullscreen: false })
  if (!init.ok) { fs.appendFileSync(outFile, 'INIT_FAIL\n'); proc.kill(); process.exit(3) }
  const load = await send('LOAD', { path: media, type: 'video', loop: (process.argv[8] || '1') !== '0', muted: true, volume: 1, speed: 1, mode: 'fill' })
  if (!load.ok) { fs.appendFileSync(outFile, 'LOAD_FAIL ' + (load.error || '') + '\n'); proc.kill(); process.exit(3) }
  await send('PLAY', {})
  for (let i = 0; i < N; i++) {
    await new Promise((r) => setTimeout(r, 1000))
    let s
    try {
      s = await send('GET_STATUS', {})
    } catch (e) {
      fs.appendFileSync(outFile, 'POLL_ERROR ' + e.message + '\n')
      break
    }
    if (!s || !s.data) { fs.appendFileSync(outFile, 'NO_DATA\n'); break }
    const t = (s.data && s.data.telemetry) || {}
    const mons = ((t.monitorPresents || []).map((m) => m.id + '=' + m.presents).join(','))
    fs.appendFileSync(outFile, [title, 'src=' + t.sourceFps, 'dec=' + f1(t.decoderFps), 'rend=' + f1(t.renderFps), 'out=' + f1(t.presentFps), 'eff=' + f1(t.effectiveVideoFps), 'drop=' + t.framesDropped, 'rep=' + t.repeatedFrames, 'wrap=' + (t.loopWraps || 0), 'cons=' + (t.framesConsumed || 0), 'decc=' + (t.framesDecoded || 0), 'mons=' + mons].join(' | ') + '\n')
  }
  await send('SHUTDOWN', {})
  proc.kill()
  process.exit(0)
}
main()
