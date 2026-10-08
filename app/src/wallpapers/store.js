import { useSyncExternalStore } from 'react'
import { createWallpaperFromType, detectFileType, isSupportedWallpaperType, PLAYLISTS, PLAYLIST_ICON_DEFAULTS, TYPE_META } from './registry'

const wallpapers = new Map()
let playlists = PLAYLISTS.map((name, i) => ({ id: `pl-${i}`, name, icon: PLAYLIST_ICON_DEFAULTS[i] ?? 'music' }))
let selectedId = null
const listeners = new Set()
let snapshot = null
let seeded = false
let cloneCounter = 0
let fileCounter = 0

function bump() {
  snapshot = null
  for (const listener of listeners) listener()
}

export function seedWallpapers() {
  if (seeded) return
  const wallpaper = createWallpaperFromType('VIDEO')
  if (wallpaper) {
    wallpapers.set(wallpaper.id, wallpaper)
    // Don't auto-select the seed: it has no filePath, and selecting it
    // makes applyWallpaperToDesktop STOP the desktop on every boot while
    // wiping lastApplied recovery state. Select only real files.
    if (wallpaper.filePath) selectedId = wallpaper.id
  }
  seeded = true
  bump()
}

export function subscribe(listener) {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

export function getSnapshot() {
  if (!snapshot) snapshot = { wallpapers: [...wallpapers.values()], playlists: [...playlists], selectedId }
  return snapshot
}

export function useWallpaperStore() {
  return useSyncExternalStore(subscribe, getSnapshot)
}

export function selectWallpaper(id) {
  if (id !== null && !wallpapers.has(id)) return
  selectedId = id
  bump()
}

export function updateWallpaperProperty(id, path, value) {
  const wallpaper = wallpapers.get(id)
  if (!wallpaper) return
  const clone = structuredClone(wallpaper)
  let target = clone
  for (let i = 0; i < path.length - 1; i += 1) target = target[path[i]]
  target[path[path.length - 1]] = value
  wallpapers.set(id, clone)
  bump()
}

export function duplicateWallpaper(id) {
  const wallpaper = wallpapers.get(id)
  if (!wallpaper) return
  const clone = structuredClone(wallpaper)
  clone.id = `${id}-${(cloneCounter += 1)}`
  clone.info = { ...clone.info, name: `${wallpaper.info.name} Copy` }
  wallpapers.set(clone.id, clone)
  selectedId = clone.id
  bump()
}

export function deleteWallpaper(id) {
  wallpapers.delete(id)
  if (selectedId === id) selectedId = null
  bump()
}

export function renamePlaylist(id, name) {
  const existing = playlists.find((p) => p.id === id)
  if (!existing) return
  playlists = playlists.map((p) => (p.id === id ? { ...p, name } : p))
  for (const wallpaper of wallpapers.values()) {
    if (wallpaper.playlist.currentPlaylist === existing.name) {
      const clone = structuredClone(wallpaper)
      clone.playlist.currentPlaylist = name
      wallpapers.set(clone.id, clone)
    }
  }
  bump()
}

export function deletePlaylist(id) {
  const existing = playlists.find((p) => p.id === id)
  if (!existing) return
  playlists = playlists.filter((p) => p.id !== id)
  for (const wallpaper of wallpapers.values()) {
    if (wallpaper.playlist.currentPlaylist === existing.name) {
      const clone = structuredClone(wallpaper)
      clone.playlist.currentPlaylist = 'None'
      wallpapers.set(clone.id, clone)
    }
  }
  bump()
}

export function setPlaylistIcon(id, icon) {
  playlists = playlists.map((p) => (p.id === id ? { ...p, icon } : p))
  bump()
}

function nextPlaylistId() {
  const max = playlists.reduce((acc, p) => {
    const n = parseInt(String(p.id ?? '').replace('pl-', ''), 10)
    return Number.isNaN(n) ? acc : Math.max(acc, n)
  }, 0)
  return `pl-${max + 1}`
}

export function createPlaylist(name, icon = 'music') {
  const id = nextPlaylistId()
  playlists = [...playlists, { id, name, icon }]
  bump()
  return id
}

export function duplicatePlaylist(id) {
  const source = playlists.find((p) => p.id === id)
  if (!source) return null
  return createPlaylist(`${source.name} Copy`, source.icon)
}

export function restorePlaylists(list) {
  playlists = list.map((p) => ({ ...p }))
  bump()
}

export async function loadWallpaperFile(file) {
  seedWallpapers()
  const fileName = typeof file === 'string' ? file : file.name
  const type = detectFileType(fileName) ?? 'IMAGE'
  if (!isSupportedWallpaperType(type)) {
    console.warn(`loadWallpaperFile: ${type} wallpapers are not supported yet (${fileName}); skipping.`)
    return null
  }
  const wallpaper = createWallpaperFromType(type)
  if (!wallpaper) return null
  wallpaper.id = `file-${(fileCounter += 1)}`
  wallpaper.info.name = fileName.replace(/\.[^.]+$/, '')
  wallpaper.info.source = fileName
  wallpaper.info.filePath = fileName
  wallpaper.info.description = `Imported ${TYPE_META[type].label.toLowerCase()} wallpaper.`
  if (typeof file !== 'string') {
    wallpaper.filePath = file.path ?? null
    if (window.rexDesktop?.getPathForFile) {
      try {
        wallpaper.filePath = (await window.rexDesktop.getPathForFile(file)) ?? wallpaper.filePath
      } catch {
        // keep fallback
      }
    }
    wallpaper.info.fileSize = `${(file.size / (1024 * 1024)).toFixed(1)} MB`
    wallpaper.info.dateAdded = new Date().toISOString().slice(0, 10)
  }
  wallpapers.set(wallpaper.id, wallpaper)
  selectedId = wallpaper.id
  bump()
  return wallpaper.id
}
