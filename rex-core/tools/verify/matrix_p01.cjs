'use strict'
// P0.1 live matrix: video -> image (must HIDE) -> video (must SHOW) ->
// STOP (must STAY visible = snap) + PLAY-after-image protocol check.
const { spawn, execFileSync } = require('node:child_process')
const exe = process.env.REX_PROBE_EXE || 'D:\\REX_doc\\rex-core\\.build\\Release\\rex-rt-test.exe'
const video = 'D:\\live wallpaper\\astral-defender-acheron.3840x2160.mp4'
const image = 'D:\\REX_doc\\e2e_a.png'
const winvis = 'C:\\Users\\Moinu\\AppData\\Local\\Temp\\opencode\\rex-verify\\winvis.ps1'
function vis(tag) {
  let out = ''
  try {
    out = execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', winvis], { encoding: 'utf8', timeout: 20000 }).trim()
  } catch (e) { out = 'WINVIS_ERROR ' + e.message.substring(0, 100) }
  console.log('[' + tag + '] ' + out.split('\n').join(' | '))
}
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
  }
})
proc.stderr.on('data', () => {})
proc.on('exit', (code) => { console.log('RUNTIME EXIT code=' + code); process.exit(code === 0 ? 0 : 2) })
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
async function main() {
  const short = (m) => JSON.stringify(m).substring(0, 140)
  console.log('INIT => ' + short(await send('INIT', { maxFps: 60, hardwareAcceleration: true, pauseOnFullscreen: false })))
  console.log('LOAD video => ' + short(await send('LOAD', { path: video, type: 'video', loop: true, muted: true, volume: 1, speed: 1, mode: 'fill' })))
  console.log('PLAY => ' + short(await send('PLAY', {})))
  await sleep(8000)
  const s1 = await send('GET_STATUS', {})
  console.log('playing: out=' + ((s1.data || {}).telemetry || {}).presentFps)
  vis('1-video-playing (expect visible=TRUE)')
  console.log('LOAD image => ' + short(await send('LOAD', { path: image, type: 'image', mode: 'fill' })))
  await sleep(2000)
  vis('2-after-image-LOAD (expect visible=FALSE = THE FIX)')
  console.log('PLAY-after-image => ' + short(await send('PLAY', {})) + ' (expect ok:false, scene empty)')
  console.log('LOAD video2 => ' + short(await send('LOAD', { path: video, type: 'video', loop: true, muted: true, volume: 1, speed: 1, mode: 'fill' })))
  console.log('PLAY2 => ' + short(await send('PLAY', {})))
  await sleep(5000)
  vis('3-video-reloaded (expect visible=TRUE)')
  console.log('STOP => ' + short(await send('STOP', {})))
  await sleep(2000)
  vis('4-after-STOP (expect visible=TRUE = snap kept)')
  console.log('SHUTDOWN => ' + short(await send('SHUTDOWN', {})))
  proc.kill()
  setTimeout(() => process.exit(0), 2000).unref()
}
main()
setTimeout(() => { console.log('GLOBAL_TIMEOUT'); process.exit(3) }, 90000).unref()
