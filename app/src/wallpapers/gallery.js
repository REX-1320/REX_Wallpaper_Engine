// Persisted gallery library bridge (P0.3). Thin wrappers over the preload
// gallery channels (WallpaperLibrary in main). Every call is optional-chained:
// outside the desktop app the bridge is absent and each helper resolves null
// so the gallery degrades to in-memory-only tiles.

import { createThumbnail, createThumbnailFallback, saveThumbnail } from './thumbnail'

export async function galleryAdd(filePath, type = 'auto', extra = undefined) {
  try {
    return await window.paiintDesktop?.gallery?.add(filePath, type, extra) ?? null
  } catch {
    return null
  }
}

export async function galleryGet(filePath) {
  try {
    return await window.paiintDesktop?.gallery?.get(filePath) ?? null
  } catch {
    return null
  }
}

export async function galleryList(filter = undefined) {
  try {
    return (await window.paiintDesktop?.gallery?.list(filter)) ?? []
  } catch {
    return []
  }
}

export async function galleryRemove(filePath) {
  try {
    return await window.paiintDesktop?.gallery?.remove(filePath) ?? false
  } catch {
    return false
  }
}

// Builds a gallery tile record from a File (thumbnail + persisted poster).
// Shared by fresh imports and post-restart restore so both paths produce
// identical records. pathHint carries the real filesystem path: restored
// Files are rebuilt from bytes and have no .path, so without the hint the
// store entry would keep a bare filename the runtime cannot resolve.
export async function buildFileRecord(f, pathHint = null, thumbUrl = null,
                                       opts = {}) {
  // skipThumbnail defers poster decoding so a large library restore can paint
  // its tiles immediately instead of blocking on N video seeks.
  //
  // P0-6: createThumbnail(file, maxSize, urlOverride) resolves a bare data-URL
  // string (or null) — NOT an object. The old call passed thumbUrl where
  // maxSize belongs (URL-as-size → NaN canvas → null poster) and read
  // thumb?.dataUrl off the string (always undefined → nothing persisted).
  const thumbDataUrl = opts.skipThumbnail
    ? null
    : ((await createThumbnail(f, 480, thumbUrl)) ?? null)
  const id = crypto.randomUUID()
  const isVideo = (f.type || '').startsWith('video/')
  const filePath = typeof pathHint === 'string' && pathHint ? pathHint : (f.path ?? null)
  let thumbPath = null
  let posterDataUrl = thumbDataUrl
  // Chromium in this window cannot decode HEVC (hardware acceleration is off
  // for the UI), so the <video> path above returns null for every HEVC file.
  // Ask the main process for a poster from the GPU worker before giving up.
  // NOT during a deferred restore: buildFileRecord(skipThumbnail) must stay
  // cheap — the restore loop decodes that file lazily (and persists the
  // result), and it is the only path that can see HEVC fail first.
  if (!posterDataUrl && !opts.skipThumbnail && isVideo && filePath) {
    const fallback = await createThumbnailFallback(filePath, 480)
    if (fallback) {
      posterDataUrl = fallback.dataUrl
      thumbPath = fallback.path
    }
  }
  if (posterDataUrl && !thumbPath) {
    try {
      thumbPath = await saveThumbnail(id, posterDataUrl)
    } catch {
      thumbPath = null
    }
  }
  return {
    id,
    name: f.name,
    size: f.size,
    date: f.lastModified,
    file: f,
    path: filePath,
    thumbDataUrl: posterDataUrl,
    thumbPath,
    // Kept so a deferred poster can be decoded later from the file:// URL.
    thumbUrl: thumbUrl ?? null,
    isVideo,
  }
}

// Reads a persisted library path back into a File for gallery restore.
// Returns { file, path, url } on success, { missing: true } when the file is
// gone (kept in the library — the drive may be temporarily unavailable).
//
// Metadata only, never bytes: main hands back a file:// URL so createThumbnail
// can decode a poster frame directly. A 4K clip is hundreds of MB, and the
// previous data-URL approach would have copied the whole thing into the
// renderer as base64.
export async function galleryRestoreFile(item) {
  try {
    const res = await window.paiintDesktop?.gallery?.readFile(item?.path)
    if (!res || res.ok !== true) return { missing: true }
    // A real File carrying the true metadata but no payload: everything
    // downstream (apply, sorting, display) reads name/size/type/mtime, and the
    // thumbnail comes from the file:// URL below.
    const file = new File([], res.name ?? item.path.split(/[\\/]/).pop(), {
      type: res.type ?? '',
      lastModified: Math.round(res.lastModified ?? Date.now()),
    })
    return { file, path: res.path ?? item.path, url: res.url ?? null }
  } catch {
    return { missing: true }
  }
}
