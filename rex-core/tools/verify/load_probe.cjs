'use strict'
// LOAD-probe: INIT then LOAD, report exactly how far the runtime gets.
const { spawn } = require('node:child_process')
const exe = process.env.REX_PROBE_EXE || 'D:\\REX_doc\\rex-core\\.build\\Release\\rex-wallpaper-runtime.exe'
const media = process.argv[2]
let nextId = 1
const proc = spawn(exe, ['--runtime'], { stdio: ['pipe', 'pipe', 'pipe'] })
let buf = Buffer.alloc(0)
const pend = new Map()
function send(cmd, params) {
  const id = nextId++
  const p = Buffer.from(JSON.stringify({ id, cmd, params }), 'utf8')
  const f = Buffer.allocUnsafe(4 + p.length)
  f.writeUInt32LE(p.length, 0); p.copy(f, 4)
  return new Promise((res) => { pend.set(id, res); proc.stdin.write(f) })
}
proc.stdout.on('data', (c) => {
  buf = Buffer.concat([buf, c])
  while (buf.length >= 4) {
    const len = buf.readUInt32LE(0)
    if (buf.length < 4 + len) return
    const msg = JSON.parse(buf.subarray(4, 4 + len).toString('utf8'))
    buf = buf.subarray(4 + len)
    if (pend.has(msg.id)) { const r = pend.get(msg.id); pend.delete(msg.id); r(msg) }
    else console.log('EVENT ' + (msg.event || JSON.stringify(msg).substring(0, 80)))
  }
})
proc.stderr.on('data', (c) => console.log('STDERR: ' + c.toString('utf8').substring(0, 300)))
proc.on('exit', (code, sig) => { console.log('EXIT code=' + code + ' sig=' + sig); process.exit(0) })
async function main() {
  console.log('sending INIT')
  const hw = process.argv[4] !== 'sw'
  const i = await send('INIT', { maxFps: 60, hardwareAcceleration: hw, pauseOnFullscreen: false })
  console.log('INIT => ' + JSON.stringify(i).substring(0, 120))
  console.log('sending LOAD ' + media)
  const t = Date.now()
  const l = await Promise.race([
    send('LOAD', { path: media, type: 'video', loop: true, muted: true, volume: 1, speed: 1, mode: 'fill' }),
    new Promise((_, rej) => setTimeout(() => rej(new Error('LOAD_TIMEOUT_15s')), 15000)),
  ]).catch((e) => ({ loadError: e.message }))
  console.log('LOAD => ' + JSON.stringify(l).substring(0, 200) + ' in ' + (Date.now() - t) + 'ms')
  if (process.argv[3] === 'play') {
    console.log('sending PLAY, watching 12s')
    const p = await send('PLAY', {})
    console.log('PLAY => ' + JSON.stringify(p).substring(0, 120))
    for (let i = 0; i < 12; i++) {
      await new Promise((r) => setTimeout(r, 1000))
      const s = await send('GET_STATUS', {})
      const tt = (s.data && s.data.telemetry) || {}
      console.log('t+' + (i + 1) + 's dec=' + tt.decoderFps + ' rend=' + tt.renderFps + ' out=' + tt.presentFps + ' drop=' + tt.framesDropped)
    }
  }
  proc.kill(); setTimeout(() => process.exit(0), 1500).unref()
}
main()
setTimeout(() => { console.log('GLOBAL_TIMEOUT'); process.exit(0) }, 30000).unref()
