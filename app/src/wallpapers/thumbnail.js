// Cached thumbnail generation for the gallery/static preview.
//
// The gallery never mounts <video> elements: at import time the renderer
// decodes ONE small poster frame (a temporary in-memory URL that is revoked
// immediately), exports it as a ~480px WebP data URL, and persists it to the
// Paiint cache via the main process. Nothing video-related is retained in
// Chromium or in the wallpaper store.

export function toFileUrl(p) {
  if (!p) return null
  return p.startsWith('file://') ? p : `file:///${String(p).split('\\').join('/')}`
}

export async function saveThumbnail(id, dataUrl) {
  const res = await window.paiintDesktop?.saveThumbnail?.(id, dataUrl)
  return res?.path ?? null
}

export async function deleteThumbnail(path) {
  await window.paiintDesktop?.deleteThumbnail?.(path)
}

// HEVC posters: this window runs without hardware acceleration (the UI keeps
// its GPU footprint at zero), and Chromium has no software HEVC decoder, so
// <video> fails with DEMUXER_ERROR_NO_SUPPORTED_STREAMS. The main process
// decodes the frame in a short-lived, hardware-accelerated Electron worker and
// returns the persisted WebP path. Resolves { dataUrl, path } or null.
export async function createThumbnailFallback(filePath, maxSize = 480) {
  if (!filePath) return null
  try {
    const res = await window.paiintDesktop?.fallbackThumbnail?.(filePath, maxSize)
    if (!res?.ok || !res.path) return null
    return { dataUrl: toFileUrl(res.path), path: res.path }
  } catch {
    return null
  }
}

export async function createThumbnailFromUrl(url, isVideo, maxSize = 480) {
  return new Promise((resolve) => {
    if (!url) return resolve(null)
    const done = (result) => resolve(result)
    // A <video> element holds a demuxer + decoder (and its GPU decode
    // surfaces) until it is collected. Gallery restore can create one per
    // library entry, so relying on GC left hundreds of MB of VRAM resident in
    // the GPU process -- and a hidden window never frees it. `release` detaches
    // the media pipeline as soon as the poster frame is encoded.
    let release = null
    let settled = false
    const finish = (result) => {
      if (settled) return
      settled = true
      const fn = release
      release = null
      if (fn) {
        try {
          fn()
        } catch { /* already detached */ }
      }
      done(result)
    }
    const canvas = document.createElement('canvas')
    const ctx = canvas.getContext('2d')
    const encode = (el) => {
      const w = el.videoWidth || el.naturalWidth || 0
      const h = el.videoHeight || el.naturalHeight || 0
      if (!w || !h) return finish(null)
      const scale = Math.min(1, maxSize / Math.max(w, h))
      canvas.width = Math.max(1, Math.round(w * scale))
      canvas.height = Math.max(1, Math.round(h * scale))
      ctx.clearRect(0, 0, canvas.width, canvas.height)
      try {
        ctx.drawImage(el, 0, 0, canvas.width, canvas.height)
      } catch {
        return finish(null)
      }
      let dataUrl
      try {
        dataUrl = canvas.toDataURL('image/webp', 0.8)
      } catch {
        try {
          dataUrl = canvas.toDataURL('image/jpeg', 0.8)
        } catch {
          return finish(null)
        }
      }
      if (!dataUrl || dataUrl.length < 100) return finish(null)
      return finish(dataUrl)
    }
    if (isVideo) {
      const video = document.createElement('video')
      video.muted = true
      video.playsInline = true
      video.preload = 'metadata'
      let fallback = null
      release = () => {
        if (fallback) clearTimeout(fallback)
        video.onloadedmetadata = null
        video.onseeked = null
        video.onerror = null
        try {
          video.pause()
        } catch { /* not playing */ }
        // Clearing src and reloading tears down the decoder immediately.
        try {
          video.removeAttribute('src')
          video.load()
        } catch { /* already torn down */ }
      }
      video.onloadedmetadata = () => {
        try {
          if (video.duration > 0.5) video.currentTime = Math.min(1, video.duration / 2)
        } catch { /* ignore */ }
      }
      video.onseeked = () => {
        if (video.readyState >= 1 && video.videoWidth) encode(video)
      }
      video.onerror = () => finish(null)
      fallback = setTimeout(() => {
        if (video.readyState >= 1 && video.videoWidth) encode(video)
        else finish(null)
      }, 2500)
      video.src = url
      video.load()
    } else {
      const img = new Image()
      img.onload = () => encode(img)
      img.onerror = () => finish(null)
      img.src = url
    }
  })
}

// Decode once and downscale to a small poster. Resolves null on failure.
//
// urlOverride lets a restored library tile point straight at the file on disk
// (file:// URL) instead of an object URL built from bytes. That matters for
// video: the bytes never have to enter the renderer at all.
export async function createThumbnail(file, maxSize = 480, urlOverride = null) {
  const url = urlOverride || URL.createObjectURL(file)
  try {
    return await createThumbnailFromUrl(
      url,
      file.type?.startsWith('video/'),
      maxSize,
    )
  } finally {
    // The object URL pinned the File for the lifetime of the document.
    // createThumbnailFromUrl has already detached the media element, so
    // revoking here cannot break an in-flight decode.
    if (!urlOverride) {
      try {
        URL.revokeObjectURL(url)
      } catch { /* already revoked */ }
    }
  }
}
