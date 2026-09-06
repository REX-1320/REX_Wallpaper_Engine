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
