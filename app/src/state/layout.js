import { useSyncExternalStore } from 'react'

// Layout preferences that more than one component needs to read and write:
// the dock's size/state (the shell owns the geometry, the gallery's toolbar
// opens the panel) and the gallery/inspector split.
//
// useAppState is per-key and one-shot: it loads on mount and never
// subscribes, so a value written by one component would not reach the other.
// This store is the shared, reactive, persisted equivalent.

const LAYOUT_DEFAULTS = {
  panelOpen: true,
  panelMinimized: false,
  panelMaximized: false,
  panelHeight: 220,
  panelPlaylistId: null,
  gallerySplitPx: null,
}

// The whole `ui` object is kept (not just the layout keys) so persisting it
// back never drops sibling keys such as `lastPage`.
let ui = { ...LAYOUT_DEFAULTS }
let loaded = false
// Keys the user changed before the persisted state finished loading. The
// load must not roll those back — otherwise a click that lands during the
// load window is silently undone (and then persisted as undone).
let localTouched = new Set()
const listeners = new Set()

const subscribe = (listener) => {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

const getSnapshot = () => ui

const emit = () => {
  for (const listener of listeners) listener()
}

function pickLayout(saved) {
  const out = {}
  for (const key of Object.keys(LAYOUT_DEFAULTS)) {
    if (saved?.[key] !== undefined) out[key] = saved[key]
  }
  return out
}

export function initLayoutState() {
  if (loaded) return
  loaded = true
  const pending = window.paiintDesktop?.appState?.get?.('ui')
  if (!pending?.then) return
  pending
    .then((saved) => {
      if (!saved) return
      // The WHOLE saved `ui` object is merged, not just the layout keys: the
      // store persists the whole object back, so dropping a sibling key
      // (lastPage, sidebarCollapsed, …) would erase it from the state file.
      const merged = { ...ui, ...saved, ...pickLayout(saved) }
      // A key the user changed while the load was in flight keeps the local
      // value — the persisted copy is older by definition.
      for (const key of localTouched) {
        if (key in ui) merged[key] = ui[key]
      }
      ui = merged
      emit()
    })
    .catch(() => {})
}

let persistTimer = null
function persist() {
  if (!loaded) return
  if (persistTimer) return
  persistTimer = setTimeout(() => {
    persistTimer = null
    window.paiintDesktop?.appState?.set?.('ui', ui)?.catch?.(() => {})
  }, 200)
}

export function setLayoutState(patch) {
  let changed = false
  for (const [key, value] of Object.entries(patch)) {
    if (ui[key] !== value) {
      changed = true
      break
    }
  }
  if (!changed) return
  for (const key of Object.keys(patch)) localTouched.add(key)
  ui = { ...ui, ...patch }
  emit()
  persist()
}

export function useLayoutState() {
  return useSyncExternalStore(subscribe, getSnapshot)
}
