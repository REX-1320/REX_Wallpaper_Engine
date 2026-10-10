// Persisted gallery library bridge (P0.3). Thin wrappers over the preload
// gallery channels (WallpaperLibrary in main). Every call is optional-chained:
// outside the desktop app the bridge is absent and each helper resolves null
// so the gallery degrades to in-memory-only tiles.

import { createThumbnail, saveThumbnail } from './thumbnail'

export async function galleryAdd(filePath, type = 'auto', extra = undefined) {
  try {
    return await window.rexDesktop?.gallery?.add(filePath, type, extra) ?? null
  } catch {
    return null
  }
}

export async function galleryGet(filePath) {
  try {
    return await window.rexDesktop?.gallery?.get(filePath) ?? null
  } catch {
    return null
  }
}

export async function galleryList(filter = undefined) {
  try {
    return (await window.rexDesktop?.gallery?.list(filter)) ?? []
  } catch {
    return []
  }
}

export async function galleryRemove(filePath) {
  try {
    return await window.rexDesktop?.gallery?.remove(filePath) ?? false
  } catch {
    return false
  }
}

// Builds a gallery tile record from a File (thumbnail + persisted poster).
// Shared by fresh imports and post-restart restore so both paths produce
// identical records. pathHint carries the real filesystem path: restored
// Files are rebuilt from bytes and have no .path, so without the hint the
// store entry would keep a bare filename the runtime cannot resolve.
export async function buildFileRecord(f, pathHint = null) {
  const thumb = await createThumbnail(f)
  const id = crypto.randomUUID()
  const thumbDataUrl = thumb?.dataUrl ?? null
  let thumbPath = null
  if (thumbDataUrl) {
    try {
      thumbPath = await saveThumbnail(id, thumbDataUrl)
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
    path: typeof pathHint === 'string' && pathHint ? pathHint : (f.path ?? null),
    thumbDataUrl,
    thumbPath,
    isVideo: (f.type || '').startsWith('video/'),
  }
}

// Reads a persisted library path back into a File for gallery restore.
// Returns { file, path } on success, { missing: true } when the file is
// gone (kept in the library — the drive may be temporarily unavailable).
export async function galleryRestoreFile(item) {
  try {
    const res = await window.rexDesktop?.gallery?.readFile(item?.path)
    if (!res || res.ok !== true) return { missing: true }
    const resp = await fetch(res.dataUrl)
    const blob = await resp.blob()
    const file = new File([blob], res.name ?? item.path.split(/[\\/]/).pop(), {
      type: blob.type,
      lastModified: Math.round(res.lastModified ?? Date.now()),
    })
    return { file, path: item.path }
  } catch {
    return { missing: true }
  }
}
