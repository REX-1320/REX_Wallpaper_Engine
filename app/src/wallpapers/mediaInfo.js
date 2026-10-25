import { useEffect, useState } from 'react'

// "Does this file have an audio track?" — answered by parsing the container
// header in the main process (app/electron/mediaInfo.cjs). The renderer
// cannot answer it: Chromium cannot decode the audio codecs these files use
// (AC-3, DTS-HD MA), so a decoder-based probe would report the movies as
// silent. The engine knows, but it only ever holds the applied scene.
//
// hasAudio === null means "unknown" (unreadable file, unknown container) and
// is never treated as silent.
const cache = new Map()

export async function getMediaInfo(filePath) {
  if (!filePath) return { container: null, hasAudio: null, size: 0 }
  const key = String(filePath).toLowerCase()
  const cached = cache.get(key)
  if (cached) return cached

  let result = { container: null, hasAudio: null, size: 0 }
  try {
    result = (await window.paiintDesktop?.getMediaInfo?.(filePath)) ?? result
  } catch {
    result = { container: null, hasAudio: null, size: 0 }
  }
  cache.set(key, result)
  return result
}

export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return null
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

export function useMediaInfo(filePath) {
  const [info, setInfo] = useState({ container: null, hasAudio: null, size: 0 })

  useEffect(() => {
    let alive = true
    if (!filePath) {
      setInfo({ container: null, hasAudio: null, size: 0 })
      return () => {
        alive = false
      }
    }
    getMediaInfo(filePath).then((result) => {
      if (alive) setInfo(result)
    })
    return () => {
      alive = false
    }
  }, [filePath])

  return info
}
