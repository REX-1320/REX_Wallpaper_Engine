'use strict'
// Per-second cadence: 1s polls x N, print pos/dec/cons/pres per second.
// Usage: node status_1s.cjs <exe> <media> [cap] [secs]
const { spawn } = require('node:child_process')
const exe = process.argv[2]
const media = process.argv[3]
const cap = parseInt(process.argv[4] || '60', 10)
const N = parseInt(process.argv[5] || '35', 10)
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
proc.on('exit', (code) => { console.log('RUNTIME_EXITED code=' + code); process.exit(2) })
function send(cmd, params) {
  const id = nextId++
  const payload = Buffer.from(JSON.stringify({ id, cmd, params: params || {} }), 'utf8')
  const frame = Buffer.allocUnsafe(4 + payload.length)
  frame.writeUInt32LE(payload.length, 0)
  payload.copy(frame, 4)
  return new Promise((resolve) => { pending.set(id, resolve); proc.stdin.write(frame) })
}
async function main() {
  await send('INIT', { maxFps: cap, hardwareAcceleration: true, pauseOnFullscreen: false })
  await send('LOAD', { path: media, type: 'video', loop: true, muted: true, volume: 1, speed: 1, mode: 'fill' })
  await send('PLAY', {})
  let pd = 0, pp = 0
  for (let i = 0; i < N; i++) {
    await new Promise((r) => setTimeout(r, 1000))
    const s = await send('GET_STATUS', {})
    const t = (s.data && s.data.telemetry) || {}
    const dd = t.framesDecoded - pd; pd = t.framesDecoded
    const dp = t.presentCount - pp; pp = t.presentCount
    console.log(`s=${i + 1} pos=${Number(s.data.position).toFixed(1)} decHz=${dd} presHz=${dp} drop=${t.framesDropped} decP50=${t.decodeMsP50} decP99=${t.decodeMsP99}`)
  }
  await send('SHUTDOWN', {})
  proc.kill()
  process.exit(0)
}
main()
