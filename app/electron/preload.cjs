const { contextBridge, ipcRenderer, webUtils } = require('electron')

contextBridge.exposeInMainWorld('rexDesktop', {
  platform: process.platform,
  isDesktop: true,
  getPathForFile: (file) => webUtils.getPathForFile(file),

  // Wallpaper apply — driven by the persistent Wallpaper Runtime.
  setDesktopWallpaper: (filePath, style) =>
    ipcRenderer.invoke('wallpaper:set', { path: filePath, style }),
  setDesktopVideo: (options) => ipcRenderer.invoke('wallpaper:video', options),
  updateWallpaper: (options) => ipcRenderer.invoke('wallpaper:update', options),
  getCapabilities: () => ipcRenderer.invoke('wallpaper:capabilities'),
  pauseWallpaper: () => ipcRenderer.invoke('wallpaper:pause'),
  resumeWallpaper: () => ipcRenderer.invoke('wallpaper:resume'),

  // Launch-at-login toggle (mirrors the tray checkbox).
  getLoginItem: () => ipcRenderer.invoke('settings:get-login-item'),
  setLoginItem: (enabled) => ipcRenderer.invoke('settings:set-login-item', { enabled }),

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

  // Persisted UI state (AppStateManager in main).
  appState: {
    get: (keyPath) => ipcRenderer.invoke('app-state:get', keyPath),
    set: (keyPath, value) => ipcRenderer.invoke('app-state:set', keyPath, value),
  },

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
  isMaximized: () => ipcRenderer.invoke('window:is-maximized'),
  onMaximized: (callback) => {
    const listener = (_event, maximized) => callback(maximized)
    ipcRenderer.on('window:maximized', listener)
    return () => ipcRenderer.removeListener('window:maximized', listener)
  },

  // Tray-driven navigation (e.g. tray "Settings" -> Settings tab). Returns an
  // unsubscribe function.
  onNavigate: (callback) => {
    const listener = (_event, page) => callback(page)
    ipcRenderer.on('navigate', listener)
    return () => ipcRenderer.removeListener('navigate', listener)
  },
})
