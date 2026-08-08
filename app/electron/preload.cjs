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

  // Runtime events: { event, data } with events READY/LOADED/PLAYING/
  // PAUSED/STOPPED/UNLOADED/ERROR. Returns an unsubscribe function.
  onWallpaperEvent: (callback) => {
    const listener = (_event, msg) => callback(msg)
    ipcRenderer.on('wallpaper:event', listener)
    return () => ipcRenderer.removeListener('wallpaper:event', listener)
  },

  showInFolder: (filePath) => ipcRenderer.invoke('shell:show-in-folder', { path: filePath }),
})
