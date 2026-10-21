// Shared poster cache.
//
// The gallery decodes one small poster frame per file (see thumbnail.js) and
// keeps it in component-local state. The playlist dock renders outside the
// gallery, so both sides need one cache keyed by the file's path — the key is
// case-insensitive because Windows paths are.

const cache = new Map()
const listeners = new Set()

// The playlist strip lives outside the gallery but renders the same posters.
// Notifying subscribers lets a chip light up the moment the gallery (or the
// GPU worker) produces a frame, instead of waiting for the next app launch.
export function subscribeThumbs(listener) {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

function notify() {
  for (const listener of listeners) {
    try {
      listener()
    } catch {
      /* a subscriber must not break thumbnails */
    }
  }
}

export function thumbKey(value) {
  if (!value) return null
  return String(value).replace(/\//g, '\\').toLowerCase()
}

export function getThumb(value) {
  const key = thumbKey(value)
  return key ? (cache.get(key) ?? null) : null
}

export function setThumb(value, entry) {
  const key = thumbKey(value)
  if (!key || !entry) return
  cache.set(key, entry)
  notify()
}

export function clearThumb(value) {
  const key = thumbKey(value)
  if (key && cache.delete(key)) notify()
}
