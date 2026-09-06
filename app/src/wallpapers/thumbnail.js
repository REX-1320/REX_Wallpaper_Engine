// Cached thumbnail generation for the gallery/static preview.
//
// The gallery never mounts <video> elements: at import time the renderer
// decodes ONE small poster frame (a temporary in-memory URL that is revoked
// immediately), exports it as a ~480px WebP data URL, and persists it to the
// REX cache via the main process. Nothing video-related is retained in
// Chromium or in the wallpaper store.

export function toFileUrl(p) {
  if (!p) return null
  return p.startsWith('file://') ? p : `file:///${String(p).split('\\').join('/')}`
}

export async function saveThumbnail(id, dataUrl) {
  const res = await window.rexDesktop?.saveThumbnail?.(id, dataUrl)
  return res?.path ?? null
}

export async function deleteThumbnail(path) {
  await window.rexDesktop?.deleteThumbnail?.(path)
}

// Decode once and downscale to a small poster. Resolves null on failure.
export function createThumbnail(file, maxSize = 480) {
  return new Promise((resolve) => {
    const isVideo = file.type?.startsWith('video/')
    const url = URL.createObjectURL(file)
    const done = (result) => {
      URL.revokeObjectURL(url)
      resolve(result)
    }

    const canvas = document.createElement('canvas')
    const ctx = canvas.getContext('2d')

    const encode = (el) => {
      const w = el.videoWidth || el.naturalWidth || 0
      const h = el.videoHeight || el.naturalHeight || 0
      if (!w || !h) return done(null)
      const scale = Math.min(1, maxSize / Math.max(w, h))
      canvas.width = Math.max(1, Math.round(w * scale))
      canvas.height = Math.max(1, Math.round(h * scale))
      ctx.clearRect(0, 0, canvas.width, canvas.height)
      try {
        ctx.drawImage(el, 0, 0, canvas.width, canvas.height)
      } catch {
        return done(null)
      }
      let dataUrl
      try {
        dataUrl = canvas.toDataURL('image/webp', 0.8)
      } catch {
        try {
          dataUrl = canvas.toDataURL('image/jpeg', 0.8)
        } catch {
          return done(null)
        }
      }
      if (!dataUrl || dataUrl.length < 100) return done(null)
      return done({ dataUrl, width: w, height: h })
    }

    if (isVideo) {
      const video = document.createElement('video')
      video.muted = true
      video.playsInline = true
      video.preload = 'metadata'
      video.onloadedmetadata = () => {
        try {
          if (video.duration > 0.5) video.currentTime = Math.min(1, video.duration / 2)
        } catch {
          // ignore — fall back to the timeout below
        }
      }
      video.onseeked = () => {
        if (video.readyState >= 1 && video.videoWidth) encode(video)
      }
      video.onerror = () => done(null)
      // Some sources never fire seeked; fall back to the first decodable frame.
      const fallback = setTimeout(() => {
        if (video.readyState >= 1 && video.videoWidth) encode(video)
        else done(null)
      }, 2500)
      video.addEventListener('seeked', () => clearTimeout(fallback))
      video.src = url
      video.load()
    } else {
      const img = new Image()
      img.onload = () => encode(img)
      img.onerror = () => done(null)
      img.src = url
    }
  })
}
