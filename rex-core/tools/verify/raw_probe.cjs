'use strict'
const { spawn } = require('node:child_process')
const exe = 'D:\\REX_doc\\rex-core\\.build\\Release\\rex-wallpaper-runtime.exe'
const proc = spawn(exe, ['--runtime'], { stdio: ['pipe', 'pipe', 'pipe'] })
proc.stdout.on('data', (c) => {
  console.log('STDOUT ' + c.length + ' bytes: ' + JSON.stringify(c.subarray(0, 120).toString('utf8')))
  process.exit(0)
})
proc.stderr.on('data', (c) => { console.log('STDERR: ' + c.toString('utf8').substring(0, 200)) })
proc.on('exit', (code) => { console.log('EXIT code=' + code); process.exit(0) })
proc.on('error', (e) => { console.log('SPAWN_ERROR ' + e.message); process.exit(0) })
setTimeout(() => {
  const p = Buffer.from(JSON.stringify({ id: 1, cmd: 'INIT', params: { maxFps: 60, hardwareAcceleration: true, pauseOnFullscreen: false } }), 'utf8')
  const f = Buffer.allocUnsafe(4 + p.length)
  f.writeUInt32LE(p.length, 0)
  p.copy(f, 4)
  proc.stdin.write(f)
  console.log('sent INIT')
}, 1500)
setTimeout(() => { console.log('TIMEOUT no data'); process.exit(0) }, 12000)
