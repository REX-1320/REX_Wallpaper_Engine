// Container-level media probe: does this file have an audio track?
//
// Why not the renderer or the engine? The renderer cannot decode HEVC (the app
// runs with hardware acceleration disabled) and Chromium cannot decode the
// audio codecs these files use (AC-3, DTS-HD MA) either — probing there
// would report "silent" for files that are not. The engine knows, but it only
// ever holds the applied scene and its binary is mid-flight, so the app reads
// the container itself: the presence of an audio track is a fact of the file
// header, not of the codec.
//
// MP4/MOV: a track is audio when its 'mdia'/'hdlr' handler is 'soun'.
// Matroska/WebM: a track is audio when its TrackType (0x83) is 2.
// Anything else is reported as unknown — never as silent.

const fs = require('node:fs/promises')

const MKV_HEAD_BYTES = 16 * 1024 * 1024

// ── MP4 / ISO-BMFF ──────────────────────────────────────────────────────────

async function readBoxHeader(read, offset, limit) {
  const header = await read(offset, 8)
  if (!header || header.length < 8) return null
  const rawSize = header.readUInt32BE(0)
  const type = header.toString('ascii', 4, 8)
  let headerSize = 8
  let size = rawSize
  if (rawSize === 1) {
    const large = await read(offset + 8, 8)
    if (!large || large.length < 8) return null
    size = Number(large.readBigUInt64BE(0))
    headerSize = 16
  } else if (rawSize === 0) {
    size = limit - offset
  }
  if (!Number.isFinite(size) || size < headerSize) return null
  return { type, headerSize, size }
}

// The handler type of a track box ('soun', 'vide', ...). null = unreadable.
async function trackHandler(read, start, end) {
  let offset = start
  while (offset + 8 <= end) {
    const box = await readBoxHeader(read, offset, end)
    if (!box) return null
    if (box.type === 'mdia') {
      let inner = offset + box.headerSize
      const innerEnd = offset + box.size
      while (inner + 8 <= innerEnd) {
        const child = await readBoxHeader(read, inner, innerEnd)
        if (!child) return null
        if (child.type === 'hdlr') {
          const payload = await read(inner + child.headerSize, 12)
          if (!payload || payload.length < 12) return null
          return payload.toString('ascii', 8, 12)
        }
        inner += child.size
      }
      return null
    }
    offset += box.size
  }
  return null
}

async function detectMp4Audio(read, size) {
  let offset = 0
  while (offset + 8 <= size) {
    const box = await readBoxHeader(read, offset, size)
    if (!box) return null
    if (box.type === 'moov') {
      const end = offset + box.size
      let cursor = offset + box.headerSize
      let hasAudio = false
      let sawTrack = false
      let unknownTrack = false
      while (cursor + 8 <= end) {
        const child = await readBoxHeader(read, cursor, end)
        if (!child) break
        if (child.type === 'trak') {
          sawTrack = true
          const handler = await trackHandler(read, cursor + child.headerSize, cursor + child.size)
          if (handler === 'soun') hasAudio = true
          else if (handler === null) unknownTrack = true
        }
        cursor += child.size
      }
      if (!sawTrack) return null
      if (hasAudio) return { container: 'mp4', hasAudio: true }
      if (unknownTrack) return { container: 'mp4', hasAudio: null }
      return { container: 'mp4', hasAudio: false }
    }
    offset += box.size
  }
  return null
}

// ── Matroska / WebM (EBML) ──────────────────────────────────────────────────

function readVint(buf, pos) {
  if (pos >= buf.length) return null
  const first = buf[pos]
  let length = 1
  let mask = 0x80
  while (length <= 8 && (first & mask) === 0) {
    mask >>= 1
    length += 1
  }
  if (length > 8 || pos + length > buf.length) return null
  let value = first & (mask - 1)
  let allOnes = (first & (mask - 1)) === mask - 1
  for (let i = 1; i < length; i += 1) {
    const byte = buf[pos + i]
    value = value * 256 + byte
    if (byte !== 0xff) allOnes = false
  }
  return { value, length, unknown: allOnes }
}

function readElement(buf, pos, end) {
  const id = readVint(buf, pos)
  if (!id) return null
  const size = readVint(buf, pos + id.length)
  if (!size) return null
  const dataStart = pos + id.length + size.length
  let dataEnd = size.unknown ? end : dataStart + size.value
  if (dataEnd > end) dataEnd = end
  if (dataEnd < dataStart) return null
  return {
    idBytes: buf.subarray(pos, pos + id.length),
    dataStart,
    dataEnd,
    next: dataEnd,
  }
}

function findElement(buf, start, end, idBytes) {
  let offset = start
  while (offset < end) {
    const element = readElement(buf, offset, end)
    if (!element) return null
    if (element.idBytes.length === idBytes.length && element.idBytes.equals(idBytes)) return element
    if (element.next <= offset) return null
    offset = element.next
  }
  return null
}

async function detectMkvAudio(read, size) {
  // Most files carry the Tracks element in the first megabyte; only fall back
  // to a bigger window when they do not (attachments before Tracks).
  for (const window of [2 * 1024 * 1024, MKV_HEAD_BYTES]) {
    const length = Math.min(size, window)
    const buf = await read(0, length)
    if (!buf || buf.length < 4) return null
    if (buf.readUInt32BE(0) !== 0x1a45dfa3) return null

    const segment = findElement(buf, 0, buf.length, Buffer.from([0x18, 0x53, 0x80, 0x67]))
    if (!segment) continue
    const tracks = findElement(buf, segment.dataStart, segment.dataEnd, Buffer.from([0x16, 0x54, 0xae, 0x6b]))
    if (!tracks) continue

    let offset = tracks.dataStart
    let hasAudio = false
    let sawTrack = false
    let unknownTrack = false
    while (offset < tracks.dataEnd) {
      const entry = readElement(buf, offset, tracks.dataEnd)
      if (!entry) break
      if (entry.idBytes.length === 1 && entry.idBytes[0] === 0xae) {
        sawTrack = true
        const type = findElement(buf, entry.dataStart, entry.dataEnd, Buffer.from([0x83]))
        if (!type || type.dataStart >= buf.length) unknownTrack = true
        else if (buf[type.dataStart] === 2) hasAudio = true
      }
      if (entry.next <= offset) break
      offset = entry.next
    }
    if (!sawTrack) continue
    if (hasAudio) return { container: 'matroska', hasAudio: true }
    if (unknownTrack) return { container: 'matroska', hasAudio: null }
    return { container: 'matroska', hasAudio: false }
  }
  return null
}

// ── Public API ──────────────────────────────────────────────────────────────

// read(offset, length) -> Promise<Buffer>
async function detectAudioTracks(read, size) {
  if (!Number.isFinite(size) || size <= 0) return { container: null, hasAudio: null }
  const mp4 = await detectMp4Audio(read, size)
  if (mp4) return mp4
  const mkv = await detectMkvAudio(read, size)
  if (mkv) return mkv
  return { container: null, hasAudio: null }
}

// Probes a file on disk. Never throws: an unreadable file is "unknown".
async function probeMediaFile(filePath) {
  let handle = null
  try {
    handle = await fs.open(filePath, 'r')
    const stat = await handle.stat()
    if (!stat.isFile()) return { container: null, hasAudio: null, size: 0 }
    const read = async (offset, length) => {
      const buffer = Buffer.alloc(length)
      const { bytesRead } = await handle.read(buffer, 0, length, offset)
      return buffer.subarray(0, bytesRead)
    }
    const result = await detectAudioTracks(read, stat.size)
    return { ...result, size: stat.size }
  } catch {
    return { container: null, hasAudio: null, size: 0 }
  } finally {
    try {
      await handle?.close()
    } catch {
      /* already closed */
    }
  }
}

module.exports = { detectAudioTracks, probeMediaFile }
