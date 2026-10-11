'use strict'
// Fake native runtime: speaks the frame protocol just enough for the
// supervisor harness. Answers INIT/LOAD/PLAY/SHUTDOWN/GET_STATUS;
// PING only when FAKE_PING=1 (default: ignore, like a pre-PING binary).
// FAKE_EXIT_AFTER_MS + FAKE_EXIT_CODE simulate crash/unexpected exit.
let buf = Buffer.alloc(0)
const pending = []
function reply(id, ok, err, data) {
  const p = Buffer.from(JSON.stringify({ id, ok, type: 'response', error: err, data }), 'utf8')
  const f = Buffer.allocUnsafe(4 + p.length)
  f.writeUInt32LE(p.length, 0); p.copy(f, 4)
  process.stdout.write(f)
}
process.stdin.on('data', (c) => {
  buf = Buffer.concat([buf, c])
  while (buf.length >= 4) {
    const len = buf.readUInt32LE(0)
    if (buf.length < 4 + len) return
    const msg = JSON.parse(buf.subarray(4, 4 + len).toString('utf8'))
    buf = buf.subarray(4 + len)
    const { id, cmd } = msg
    if (cmd === 'INIT') { reply(id, true, '', {}); process.stdout.write(Buffer.concat([frame({ event: 'READY', data: {} })])) }
    else if (cmd === 'SHUTDOWN') { reply(id, true, '', {}); setTimeout(() => process.exit(0), 100).unref() }
    else if (cmd === 'PING') { if (process.env.FAKE_PING === '1') reply(id, true, '', {}) }
    else reply(id, true, '', {})
  }
})
function frame(obj) {
  const p = Buffer.from(JSON.stringify(obj), 'utf8')
  const f = Buffer.allocUnsafe(4 + p.length)
  f.writeUInt32LE(p.length, 0); p.copy(f, 4)
  return f
}
const after = parseInt(process.env.FAKE_EXIT_AFTER_MS || '0', 10)
if (after > 0) {
  setTimeout(() => process.exit(parseInt(process.env.FAKE_EXIT_CODE || '2', 10)), after).unref()
}
