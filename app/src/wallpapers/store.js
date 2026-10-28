import { useSyncExternalStore } from 'react'
import { createWallpaperFromType, detectFileType, isSupportedWallpaperType, PLAYLISTS, PLAYLIST_ICON_DEFAULTS, TYPE_META } from './registry'
import { applySettings, isLiveProperty, pickLiveSettings } from './wallpaperSettings'
import { applyDefaults, sanitizeDefaults } from '../settings/defaults'
import { playlistScopeForApply } from './playlist/scope'

// The defaults the Settings page owns, read once per import. A missing or
// corrupt entry falls back to the built-in baseline.
async function loadDefaults() {
  try {
    const raw = await window.paiintDesktop?.appState?.get?.('defaults')
    return sanitizeDefaults(raw)
  } catch {
    return null
  }
}

const wallpapers = new Map()
// 'None' (pl-0) is the "no active playlist" sentinel, not a real container.
// 'Favorites' (pl-1) is virtual: its members are derived from the favorite flag.
const VIRTUAL_PLAYLIST_NAME = 'Favorites'
const BUILTIN_PLAYLIST_IDS = new Set(PLAYLISTS.map((_, i) => `pl-${i}`))

let playlists = PLAYLISTS.map((name, i) => ({
  id: `pl-${i}`,
  name,
  icon: PLAYLIST_ICON_DEFAULTS[i] ?? 'music',
  members: [],
  virtual: name === VIRTUAL_PLAYLIST_NAME,
}))
let selectedId = null
let appliedId = null
// True when the runtime re-applied `appliedId` at boot (main replayed
// last-wallpaper.json). DesktopApply must then adopt the scene instead of
// issuing a second LOAD — selecting the tile must stay a silent preview.
let externallyApplied = false
let lastAppliedPath = null
const listeners = new Set()
// Applies are announced synchronously, before React re-renders. The playlist
// engine needs that: it must stop main's previous rotation (and restart the
// new playlist's timer) at the click, not 150 ms later when the mirrored plan
// happens to be rebuilt. The store cannot import the engine (the engine
// imports the store), so this is the one-way hook.
const appliedListeners = new Set()
let snapshot = null
let seeded = false
let cloneCounter = 0
let fileCounter = 0

function bump() {
  snapshot = null
  for (const listener of listeners) listener()
}

function notifyWallpaperApplied(payload) {
  for (const listener of appliedListeners) {
    try {
      listener(payload)
    } catch {
      // An engine listener must never break the apply itself.
    }
  }
}

// Registers the one listener that mirrors "what is on the desktop" into the
// playlist engine. Returns an unsubscribe function.
export function subscribeWallpaperApplied(listener) {
  appliedListeners.add(listener)
  return () => appliedListeners.delete(listener)
}

// ─── Per-wallpaper live settings persistence ───────────────────────────────
// The six engine-backed properties are written into the gallery library entry
// for the file (debounced: sliders fire per pixel). Without this the settings
// reset to defaults on every relaunch — the inspector looked live but forgot.
const SETTINGS_PERSIST_DELAY = 400
const settingsPersistTimers = new Map()
const settingsPersistPayloads = new Map()

function scheduleSettingsPersist(wallpaper) {
  const filePath = wallpaper?.filePath
  if (!filePath) return
  const gallery = window.paiintDesktop?.gallery
  if (typeof gallery?.update !== 'function') return

  settingsPersistPayloads.set(filePath, pickLiveSettings(wallpaper))
  if (settingsPersistTimers.has(filePath)) return

  settingsPersistTimers.set(
    filePath,
    setTimeout(() => {
      settingsPersistTimers.delete(filePath)
      const settings = settingsPersistPayloads.get(filePath)
      settingsPersistPayloads.delete(filePath)
      if (!settings) return
      try {
        gallery.update(filePath, { settings })?.catch?.(() => {})
      } catch {
        // Bridge went away mid-write; the in-memory value is still correct.
      }
    }, SETTINGS_PERSIST_DELAY)
  )
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
  if (!snapshot) {
    snapshot = {
      wallpapers: [...wallpapers.values()],
      playlists: [...playlists],
      selectedId,
      appliedId,
      externallyApplied,
    }
  }
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

// Applies to the desktop. Unlike selectWallpaper (single click / preview),
// this is the only selection change that triggers DesktopApply.
//
// `source` decides whether the desktop is governed by a playlist (see
// playlistScopeForApply): a gallery apply opts out explicitly even when the
// file is a playlist member, a playlist apply names its playlist, and an
// engine rotation leaves the tag alone.
export function applyWallpaper(id, { source = 'gallery', playlist = null } = {}) {
  if (id !== null && !wallpapers.has(id)) return
  if (id !== null) {
    const scope = playlistScopeForApply(source, playlist)
    if (scope) {
      const clone = structuredClone(wallpapers.get(id))
      clone.playlist = { ...clone.playlist, ...scope }
      wallpapers.set(id, clone)
    }
  }
  selectedId = id
  appliedId = id
  externallyApplied = false
  const applied = id !== null ? wallpapers.get(id) : null
  // Announce AFTER bump(): bump schedules the debounced plan sync (150 ms),
  // the listener immediately schedules a 0 ms one. Doing it the other way
  // round let the debounce replace the immediate sync.
  bump()
  notifyWallpaperApplied({
    id,
    path: applied ? applied.filePath ?? applied.info?.filePath ?? applied.info?.source ?? null : null,
    source,
    playlist,
  })
}

export function updateWallpaperProperty(id, path, value) {
  const wallpaper = wallpapers.get(id)
  if (!wallpaper) return
  const clone = structuredClone(wallpaper)
  let target = clone
  for (let i = 0; i < path.length - 1; i += 1) target = target[path[i]]
  target[path[path.length - 1]] = value
  // The playlist a wallpaper starts with ('None') is also the value the user
  // picks to turn rotation off. Mark the pick so the engine can tell the two
  // apart - it is runtime-only, like the name itself (the engine falls back
  // to the playlist that holds the file after a restart).
  if (path.length === 2 && path[0] === 'playlist' && path[1] === 'currentPlaylist') target.explicit = true
  wallpapers.set(id, clone)
  if (isLiveProperty(path)) scheduleSettingsPersist(clone)
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
  if (appliedId === id) {
    appliedId = null
    externallyApplied = false
  }
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
  persistPlaylists()
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
  persistPlaylists()
}

export function setPlaylistIcon(id, icon) {
  playlists = playlists.map((p) => (p.id === id ? { ...p, icon } : p))
  bump()
  persistPlaylists()
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
  playlists = [...playlists, { id, name, icon, members: [], virtual: name === VIRTUAL_PLAYLIST_NAME }]
  bump()
  persistPlaylists()
  return id
}

export function duplicatePlaylist(id) {
  const source = playlists.find((p) => p.id === id)
  if (!source) return null
  // A copy starts empty: the membership is the user's to curate.
  return createPlaylist(`${source.name} Copy`, source.icon)
}

export function restorePlaylists(list) {
  playlists = list.map((p) => ({ ...p, members: Array.isArray(p.members) ? p.members : [] }))
  bump()
}

// ─── Playlist membership ────────────────────────────────────────────────────
// A playlist owns its members. Membership is keyed by the file's absolute
// path: the store's own ids come from a counter and are reassigned on every
// restart, so an id-keyed list would scramble across sessions.

export function playlistMemberKey(path) {
  if (!path) return null
  return String(path).replace(/\//g, '\\').toLowerCase()
}

export function getPlaylistMembers(id) {
  return playlists.find((p) => p.id === id)?.members ?? []
}

export function isPlaylistMember(id, path) {
  const key = playlistMemberKey(path)
  if (!key) return false
  return getPlaylistMembers(id).some((m) => playlistMemberKey(m) === key)
}

export function addPlaylistMembers(id, paths) {
  const list = playlists.find((p) => p.id === id)
  if (!list) return
  const seen = new Set(list.members.map(playlistMemberKey))
  const next = [...list.members]
  let changed = false
  for (const p of paths) {
    const key = playlistMemberKey(p)
    if (!key || seen.has(key)) continue
    seen.add(key)
    next.push(p)
    changed = true
  }
  if (!changed) return
  playlists = playlists.map((p) => (p.id === id ? { ...p, members: next } : p))
  bump()
  persistPlaylists()
}

export function removePlaylistMember(id, path) {
  const list = playlists.find((p) => p.id === id)
  if (!list) return
  const key = playlistMemberKey(path)
  if (!key) return
  const next = list.members.filter((m) => playlistMemberKey(m) !== key)
  if (next.length === list.members.length) return
  playlists = playlists.map((p) => (p.id === id ? { ...p, members: next } : p))
  bump()
  persistPlaylists()
}

export function reorderPlaylistMember(id, from, to) {
  const list = playlists.find((p) => p.id === id)
  if (!list) return
  const members = [...list.members]
  if (from < 0 || from >= members.length || to < 0 || to >= members.length || from === to) return
  const [moved] = members.splice(from, 1)
  members.splice(to, 0, moved)
  playlists = playlists.map((p) => (p.id === id ? { ...p, members } : p))
  bump()
  persistPlaylists()
}

// ─── Playlist persistence ───────────────────────────────────────────────────
// Only playlists the user touched are written: the built-ins are re-seeded on
// every start, so persisting them verbatim would freeze their names and icons.
let playlistPersistTimer = null
function persistPlaylists() {
  if (playlistPersistTimer) return
  playlistPersistTimer = setTimeout(() => {
    playlistPersistTimer = null
    const payload = playlists.filter((p) => {
      if (p.name === 'None') return false
      if (!BUILTIN_PLAYLIST_IDS.has(p.id)) return true // user-created
      if (p.members.length > 0) return true // has members
      // A built-in the user renamed or re-iconed still has to be written.
      const index = Number(String(p.id).replace('pl-', ''))
      return p.name !== PLAYLISTS[index] || p.icon !== (PLAYLIST_ICON_DEFAULTS[index] ?? 'music')
    }).map((p) => ({ id: p.id, name: p.name, icon: p.icon, members: p.members }))
    window.paiintDesktop?.appState?.set?.('playlistEngine.playlists', payload)?.catch?.(() => {})
  }, 250)
}

export function hydratePlaylists(saved) {
  if (!Array.isArray(saved) || saved.length === 0) return
  const byId = new Map(saved.map((p) => [p.id, p]))
  const merged = playlists.map((p) => {
    const s = byId.get(p.id)
    if (!s) return p
    byId.delete(p.id)
    return {
      ...p,
      name: s.name ?? p.name,
      icon: s.icon ?? p.icon,
      members: Array.isArray(s.members) ? s.members.filter((m) => typeof m === 'string') : p.members,
    }
  })
  // Playlists the user created (not one of the built-ins) come back too.
  for (const s of byId.values()) {
    merged.push({
      id: s.id,
      name: s.name ?? 'Playlist',
      icon: s.icon ?? 'music',
      members: Array.isArray(s.members) ? s.members.filter((m) => typeof m === 'string') : [],
      virtual: s.name === VIRTUAL_PLAYLIST_NAME,
    })
  }
  playlists = merged
  bump()
}

// Loads the persisted playlists (definitions + membership) into the store.
// Called once at startup, next to the layout state.
export function initPlaylistState() {
  const pending = window.paiintDesktop?.appState?.get?.('playlistEngine.playlists')
  if (!pending?.then) return
  pending
    .then((saved) => hydratePlaylists(saved))
    .catch(() => {})
    .finally(() => {
      playlistsHydrated = true
    })
}

// The engine must not decide that "no playlist holds this file" until the
// persisted membership has actually loaded: an empty built-in list looks the
// same as a real "not a member" and would clear a valid rotation plan on a
// cold start.
let playlistsHydrated = false
export function arePlaylistsHydrated() {
  return playlistsHydrated
}

// Adopts the wallpaper main re-applied at startup. Without this the store
// believed nothing was on the desktop: the inspector offered "Apply" for the
// wallpaper that was already showing, and live SET_* edits were never sent.
// Main replays last-wallpaper.json while the renderer is already mounting, so
// the first query can legitimately return null — retry lazily on file load.
let lastAppliedRequest = null

async function fetchLastAppliedPath() {
  if (lastAppliedPath) return lastAppliedPath
  if (!lastAppliedRequest) {
    lastAppliedRequest = (async () => {
      try {
        const last = await window.paiintDesktop?.getLastApplied?.()
        if (typeof last?.path === 'string' && last.path) lastAppliedPath = last.path
      } catch {
        // bridge gone or runtime never applied anything
      }
      return lastAppliedPath
    })().finally(() => {
      lastAppliedRequest = null
    })
  }
  return lastAppliedRequest
}

export function initLastAppliedState() {
  if (!window.paiintDesktop?.getLastApplied) return
  fetchLastAppliedPath()
    .then((path) => {
      if (!path) return
      // Materialise the file if the gallery has not restored it yet: the
      // inspector's "Applied" state keys off appliedId, and live SET_* edits
      // need a record even when the user never opened this tile.
      adoptAppliedPath(path).catch(() => {})
    })
    .catch(() => {})
}

function syncAppliedId() {
  if (!lastAppliedPath || appliedId) return
  const bootKey = playlistMemberKey(lastAppliedPath)
  if (!bootKey) return
  for (const wallpaper of wallpapers.values()) {
    if (playlistMemberKey(wallpaper.filePath) === bootKey) {
      appliedId = wallpaper.id
      externallyApplied = true
      return
    }
  }
}

// Adopt the scene the runtime is showing as "on desktop" WITHOUT reloading it.
//
// Main applies playlist members itself now; when the window is open it must
// not send a second LOAD for a file that is already playing (DesktopApply
// keys its effect on appliedId). Setting externallyApplied tells DesktopApply
// to update its baseline and skip the apply. The same helper heals the cold
// start, where the runtime reports the restored wallpaper before the store
// has materialised its record.
export async function adoptAppliedPath(path, opts = {}) {
  const target = typeof path === 'string' ? path : ''
  if (!target) return null
  let id = null
  try {
    id = await ensureWallpaperForPath(target, { keepSelection: true })
  } catch {
    return null
  }
  if (!id) return null
  lastAppliedPath = target
  appliedId = id
  externallyApplied = true
  if (!opts.silent) bump()
  // Same ordering as applyWallpaper: the 0 ms plan sync must win over the
  // debounced one a non-silent bump schedules.
  notifyWallpaperApplied({ id, path: target, source: 'external', playlist: null })
  return id
}

export async function loadWallpaperFile(file, pathHint = null, opts = {}) {
  seedWallpapers()
  const fileName = typeof file === 'string' ? file : file.name
  const type = detectFileType(fileName) ?? 'IMAGE'
  if (!isSupportedWallpaperType(type)) {
    console.warn(`loadWallpaperFile: ${type} wallpapers are not supported yet (${fileName}); skipping.`)
    return null
  }
  let wallpaper = createWallpaperFromType(type)
  if (!wallpaper) return null
  wallpaper.id = `file-${(fileCounter += 1)}`
  wallpaper.info.name = fileName.replace(/\.[^.]+$/, '')
  wallpaper.info.source = fileName
  wallpaper.info.filePath = fileName
  wallpaper.info.description = `Imported ${TYPE_META[type].label.toLowerCase()} wallpaper.`
  // pathHint wins: restored Files are rebuilt from bytes and carry no
  // .path, so without it the entry keeps a bare filename the runtime
  // cannot resolve (tile selects, wall never changes — silent no-op).
  if (typeof pathHint === 'string' && pathHint) {
    wallpaper.filePath = pathHint
    wallpaper.info.filePath = pathHint
  } else if (typeof file === 'string') {
    wallpaper.filePath = file
    wallpaper.info.filePath = file
  } else {
    wallpaper.filePath = file.path ?? null
    if (!wallpaper.filePath && window.paiintDesktop?.getPathForFile) {
      try {
        wallpaper.filePath = (await window.paiintDesktop.getPathForFile(file)) ?? wallpaper.filePath
      } catch {
        // keep fallback
      }
    }
    if (wallpaper.filePath) wallpaper.info.filePath = wallpaper.filePath
    wallpaper.info.fileSize = `${(file.size / (1024 * 1024)).toFixed(1)} MB`
    wallpaper.info.dateAdded = new Date().toISOString().slice(0, 10)
  }
  // The defaults are the BASELINE for a NEWLY imported file: it starts from
  // them, and anything the user saved for that file (below) still wins.
  // A file that is already in the library keeps its own values - the
  // defaults are for new wallpapers, not a retroactive re-style.
  if (opts.isNew) {
    const defaults = await loadDefaults()
    if (defaults) wallpaper = applyDefaults(wallpaper, defaults)
  }
  // Restore the six live settings saved for this file (fit mode, volume,
  // mute, loop, speed, output). Fresh imports race the gallery.add write, so
  // a miss here simply means "defaults".
  if (wallpaper.filePath) {
    try {
      const stored = await window.paiintDesktop?.gallery?.get?.(wallpaper.filePath)
      if (stored?.settings) {
        const hydrated = applySettings(wallpaper, stored.settings)
        if (hydrated) wallpaper = hydrated
      }
    } catch {
      // Bridge absent or unreadable entry — keep defaults.
    }
  }
  // One record per file. Boot adoption, playlist materialisation and a tile
  // click all resolve the same path, often concurrently; without this check
  // each caller created its own record (measured: three records for one file).
  // appliedId then pointed at a different record than the tile the user
  // selected, so the inspector lost its "On desktop" state after a background
  // rotation even though the right file was playing.
  const resolvedPath = wallpaper.filePath ?? wallpaper.info?.filePath ?? null
  if (resolvedPath) {
    const key = playlistMemberKey(resolvedPath)
    for (const existing of wallpapers.values()) {
      const candidate = existing.filePath ?? existing.info?.filePath ?? existing.info?.source
      if (playlistMemberKey(candidate) === key) {
        if (!opts.keepSelection && selectedId !== existing.id) {
          selectedId = existing.id
          bump()
        }
        return existing.id
      }
    }
  }
  await fetchLastAppliedPath()
  wallpapers.set(wallpaper.id, wallpaper)
  syncAppliedId()
  // The playlist engine materialises a playlist's files in the background:
  // they must not steal the selection from the user while it does.
  if (!opts.keepSelection) selectedId = wallpaper.id
  bump()
  return wallpaper.id
}

// Finds (or creates) the store record for a file path. The gallery's tiles
// and the playlist dock both need to focus a file that has not been loaded
// into the store yet — records are created lazily, on first select/apply.
// In-flight creations keyed by normalized path: three callers (boot adoption,
// playlist hydration, a tile click) can ask for the same file in the same tick,
// and each would otherwise create its own record.
const pendingPathLoads = new Map()

export async function ensureWallpaperForPath(path, opts = {}) {
  if (!path) return null
  const key = playlistMemberKey(path)
  for (const wallpaper of wallpapers.values()) {
    const candidate = wallpaper.filePath ?? wallpaper.info?.filePath ?? wallpaper.info?.source
    if (playlistMemberKey(candidate) === key) return wallpaper.id
  }
  const pending = pendingPathLoads.get(key)
  if (pending) return pending
  const name = String(path).split(/[\\/]/).pop()
  if (!name) return null
  const load = loadWallpaperFile(name, path, opts).finally(() => {
    pendingPathLoads.delete(key)
  })
  pendingPathLoads.set(key, load)
  return load
}
