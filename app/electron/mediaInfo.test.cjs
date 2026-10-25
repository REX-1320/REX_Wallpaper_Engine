const test = require('node:test')
const assert = require('node:assert/strict')

const { detectAudioTracks, probeMediaFile } = require('./mediaInfo.cjs')

// ── Builders for synthetic containers ───────────────────────────────────────

function box(type, payload) {
  const header = Buffer.alloc(8)
  header.writeUInt32BE(8 + payload.length, 0)
  header.write(type, 4, 'ascii')
  return Buffer.concat([header, payload])
}

function hdlr(handlerType) {
  const payload = Buffer.alloc(24)
  payload.writeUInt32BE(0, 0) // version + flags
  payload.writeUInt32BE(0, 4) // pre_defined
  payload.write(handlerType, 8, 'ascii')
  return payload
}

function trak(handlerType) {
  return box('trak', box('mdia', box('hdlr', hdlr(handlerType))))
}

function mp4(handlers) {
  const tracks = handlers.map((h) => trak(h))
  return Buffer.concat([
    box('ftyp', Buffer.alloc(8)),
    box('moov', Buffer.concat(tracks)),
    box('mdat', Buffer.alloc(32)),
  ])
}

// EBML: a variable-size integer for element sizes.
function vint(value) {
  if (value < 0x7f) return Buffer.from([0x80 | value])
  if (value < 0x3fff) {
    const b = Buffer.alloc(2)
    b.writeUInt16BE(0x4000 | value, 0)
    return b
  }
  const b = Buffer.alloc(4)
  b.writeUInt32BE(0x20000000 | value, 0)
  return b
}

function ebml(id, payload) {
  return Buffer.concat([Buffer.from(id), vint(payload.length), payload])
}

function mkv(trackTypes) {
  const entries = trackTypes.map((type) =>
    ebml([0xae], Buffer.concat([ebml([0x83], Buffer.from([type])), ebml([0x86], Buffer.from('V_MPEG4/ISO/AVC'))])),
  )
  return Buffer.concat([
    ebml([0x1a, 0x45, 0xdf, 0xa3], Buffer.alloc(8)),
    ebml([0x18, 0x53, 0x80, 0x67], ebml([0x16, 0x54, 0xae, 0x6b], Buffer.concat(entries))),
  ])
}

function reader(buffer) {
  return async (offset, length) => buffer.subarray(offset, offset + length)
}

// ── Tests ───────────────────────────────────────────────────────────────────

test('an MP4 with a sound track reports audio', async () => {
  const result = await detectAudioTracks(reader(mp4(['vide', 'soun'])), 1000)
  assert.equal(result.container, 'mp4')
  assert.equal(result.hasAudio, true)
})

test('an MP4 with only a video track reports no audio', async () => {
  const result = await detectAudioTracks(reader(mp4(['vide'])), 1000)
  assert.equal(result.container, 'mp4')
  assert.equal(result.hasAudio, false)
})

test('a Matroska file with an audio track reports audio', async () => {
  const result = await detectAudioTracks(reader(mkv([1, 2])), 1000)
  assert.equal(result.container, 'matroska')
  assert.equal(result.hasAudio, true)
})

test('a Matroska file with only a video track reports no audio', async () => {
  const result = await detectAudioTracks(reader(mkv([1])), 1000)
  assert.equal(result.container, 'matroska')
  assert.equal(result.hasAudio, false)
})

test('an unknown container is reported as unknown, never as silent', async () => {
  const result = await detectAudioTracks(reader(Buffer.from('not a media file at all')), 24)
  assert.equal(result.hasAudio, null)
  assert.equal(result.container, null)
})

test('a truncated container is reported as unknown', async () => {
  const result = await detectAudioTracks(reader(mp4(['vide', 'soun']).subarray(0, 12)), 12)
  assert.equal(result.hasAudio, null)
})

test('probing a real file reports its size and its audio tracks', async () => {
  const fs = require('node:fs/promises')
  const os = require('node:os')
  const path = require('node:path')

  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'paiint-media-'))
  const file = path.join(dir, 'clip.mp4')
  const data = mp4(['vide', 'soun'])
  await fs.writeFile(file, data)
  try {
    const result = await probeMediaFile(file)
    assert.equal(result.hasAudio, true)
    assert.equal(result.container, 'mp4')
    assert.equal(result.size, data.length)
  } finally {
    await fs.rm(dir, { recursive: true, force: true })
  }
})

test('probing a missing file is unknown, never silent', async () => {
  const result = await probeMediaFile('D:/definitely-not-here/paiint-missing.mp4')
  assert.equal(result.hasAudio, null)
  assert.equal(result.size, 0)
})
