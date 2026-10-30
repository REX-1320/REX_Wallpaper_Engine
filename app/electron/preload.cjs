const { contextBridge, ipcRenderer, webUtils } = require('electron')

contextBridge.exposeInMainWorld('paiintDesktop', {
  getPathForFile: (file) => webUtils.getPathForFile(file),

  // Wallpaper apply — driven by the persistent Wallpaper Runtime.
  setDesktopWallpaper: (filePath, style) =>
    ipcRenderer.invoke('wallpaper:set', { path: filePath, style }),
  setDesktopVideo: (options) => ipcRenderer.invoke('wallpaper:video', options),
  updateWallpaper: (options) => ipcRenderer.invoke('wallpaper:update', options),
  getCapabilities: () => ipcRenderer.invoke('wallpaper:capabilities'),
  // Transport: pause/resume the applied wallpaper (tray + Inspector).
  pauseWallpaper: () => ipcRenderer.invoke('wallpaper:pause'),
  resumeWallpaper: () => ipcRenderer.invoke('wallpaper:resume'),
  // Playlist "stop": end the rotation and leave the last video frame on the
  // desktop as a still (the runtime captures + applies it before it exits).
  stopWallpaper: () => ipcRenderer.invoke('wallpaper:stop'),
  // Scene main re-applied at boot (last-wallpaper.json); the inspector adopts
  // it as "on desktop" without reloading.
  getLastApplied: () => ipcRenderer.invoke('wallpaper:last-applied'),
  // Container probe: does the file have an audio track?
  getMediaInfo: (filePath) => ipcRenderer.invoke('wallpaper:media-info', { path: filePath }),
  // The engine's own telemetry (GET_STATUS) for the Settings page. Reports
  // `available: false` when the runtime is not running - it never starts one.
  getStatus: () => ipcRenderer.invoke('wallpaper:status'),

  // Runtime events: { event, data } with events READY/LOADED/PLAYING/
  // PAUSED/STOPPED/UNLOADED/ERROR. Returns an unsubscribe function.
  onWallpaperEvent: (callback) => {
    const listener = (_event, msg) => callback(msg)
    ipcRenderer.on('wallpaper:event', listener)
    return () => ipcRenderer.removeListener('wallpaper:event', listener)
  },

  showInFolder: (filePath) => ipcRenderer.invoke('shell:show-in-folder', { path: filePath }),

  // Cached gallery thumbnails.
  saveThumbnail: (id, dataUrl) => ipcRenderer.invoke('wallpaper:save-thumbnail', { id, dataUrl }),
  deleteThumbnail: (path) => ipcRenderer.invoke('wallpaper:delete-thumbnail', { path }),
  // HEVC posters: a short-lived, GPU-enabled process decodes one frame when
  // this software-composited window cannot (see thumb-worker.cjs).
  fallbackThumbnail: (filePath, maxSize) =>
    ipcRenderer.invoke('wallpaper:fallback-thumbnail', { path: filePath, maxSize }),

  // Persisted UI state (AppStateManager in main).
  appState: {
    get: (keyPath) => ipcRenderer.invoke('app-state:get', keyPath),
    set: (keyPath, value) => ipcRenderer.invoke('app-state:set', keyPath, value),
  },

  // Local-only test-mode recorder. status() is safe to poll; openFolder() only
  // opens the folder main reports.
  diagnostics: {
    status: () => ipcRenderer.invoke('diagnostics:status'),
    openFolder: () => ipcRenderer.invoke('diagnostics:open-folder'),
  },

  // App-level settings (the Settings page).
  loginItem: (enabled) => ipcRenderer.invoke('app:login-item', { enabled }),
  getPaths: () => ipcRenderer.invoke('app:paths'),
  openPath: (target) => ipcRenderer.invoke('app:open-path', { target }),
  readLegal: (target) => ipcRenderer.invoke('app:read-legal', { target }),
  clearPosters: () => ipcRenderer.invoke('app:clear-posters'),

  // Persisted gallery library (WallpaperLibrary in main). add() is the
  // authoritative persisted-path dedup: same normalized path returns the
  // existing entry (lastUsed bumped) instead of a duplicate.
  gallery: {
    add: (filePath, type, extra) => ipcRenderer.invoke('gallery.add', filePath, type, extra),
    get: (filePath) => ipcRenderer.invoke('gallery.get', filePath),
    list: (filter) => ipcRenderer.invoke('gallery.list', filter),
    remove: (filePath) => ipcRenderer.invoke('gallery.remove', filePath),
    update: (filePath, updates) => ipcRenderer.invoke('gallery.update', filePath, updates),
    readFile: (filePath) => ipcRenderer.invoke('gallery.read-file', { path: filePath }),
  },

  // Custom title bar (traffic lights).
  minimize: () => ipcRenderer.send('window:minimize'),
  toggleMaximize: () => ipcRenderer.send('window:toggle-maximize'),
  close: () => ipcRenderer.send('window:close'),
  // Playlist rotation: the renderer mirrors the active plan (membership +
  // order + live settings) into main, which owns Timer and the native ENDED
  // trigger. This is what lets a closed window (or a --background login start)
  // keep rotating. onPlaylistApplied reports a rotation back to the UI.
  syncPlaylistPlan: (plan) => ipcRenderer.invoke('playlist:sync-plan', plan),
  getPlaylistSchedulerState: () => ipcRenderer.invoke('playlist:scheduler-state'),
  onPlaylistApplied: (callback) => {
    const listener = (_event, msg) => callback(msg)
    ipcRenderer.on('playlist:applied', listener)
    return () => ipcRenderer.removeListener('playlist:applied', listener)
  },
  isMaximized: () => ipcRenderer.invoke('window:is-maximized'),
  onMaximized: (callback) => {
    const listener = (_event, maximized) => callback(maximized)
    ipcRenderer.on('window:maximized', listener)
    return () => ipcRenderer.removeListener('window:maximized', listener)
  },

  // Window move / maximize / Aero Snap need no IPC: the titlebar strip is a real
  // `-webkit-app-region: drag` region and the window is not transparent, so the
  // OS caption handles all of it natively (same path as Windows Terminal).

  // Tray-driven navigation (e.g. tray "Settings" -> Settings tab). Returns an
  // unsubscribe function.
  onNavigate: (callback) => {
    const listener = (_event, page) => callback(page)
    ipcRenderer.on('navigate', listener)
    // The listener is mounted: main may now deliver a destination queued by
    // the tray (e.g. "Settings" on a freshly created window). did-finish-load
    // is too early -- React registers this effect after the document loads.
    ipcRenderer.send('renderer:navigate-ready')
    return () => ipcRenderer.removeListener('navigate', listener)
  },
})

// Renderer liveness heartbeat for the supervisor watchdog (audit 5.10 #9):
// a stale stream while the window is visible means the renderer is hung or
// dead and the main process reloads it. Hidden/minimized renderers get their
// timers throttled — the watchdog skips those and re-baselines on show.
setInterval(() => {
  try {
    ipcRenderer.send('renderer:ping')
  } catch {
    // renderer is tearing down
  }
}, 2000)

// Main-initiated liveness probe (Supervisor._rendererTick). Page timers can be
// throttled while the window is occluded even though win.isVisible() is true,
// so main asks directly before treating a quiet heartbeat as a hang. Echo the
// token so a late reply cannot satisfy a newer probe.
ipcRenderer.on('renderer:probe', (_event, token) => {
  try {
    ipcRenderer.send('renderer:probe-reply', token)
  } catch {
    // renderer is tearing down
  }
})
