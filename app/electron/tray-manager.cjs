'use strict'

// ─── TrayManager ────────────────────────────────────────────────────────────
// Owns the Windows system tray for Paiint. Pure main-process concern; the
// renderer never touches Tray/Menu. Renderer-visible behavior is exposed
// only through the narrow IPC API (see main.cjs / preload.cjs).

const { Tray, Menu, nativeImage } = require('electron')
const path = require('node:path')
const fs = require('node:fs')

const TRAY_ICON_PATH = path.join(__dirname, '..', 'assets', 'paiint-tray.ico')

class TrayManager {
  constructor({ getWindow, onShowWindow, onQuit, onPauseWallpaper, onResumeWallpaper, isLoginItemEnabled, onToggleLoginItem }) {
    this.tray = null
    this.menu = null
    this.getWindow = getWindow
    this.onShowWindow = onShowWindow
    this.onQuit = onQuit
    this.onPauseWallpaper = onPauseWallpaper
    this.onResumeWallpaper = onResumeWallpaper
    this.isLoginItemEnabled = isLoginItemEnabled
    this.onToggleLoginItem = onToggleLoginItem
    this.wallpaperPaused = false
    this.runtimeAvailable = true
  }

  _icon() {
    if (fs.existsSync(TRAY_ICON_PATH)) return nativeImage.createFromPath(TRAY_ICON_PATH)
    return nativeImage.createEmpty()
  }

  create() {
    if (this.tray) return
    this.tray = new Tray(this._icon())
    this.tray.setToolTip('Paiint')
    this.rebuildMenu()

    // Double-click (or single-click) restores the window.
    this.tray.on('double-click', () => this.showWindow())
    this.tray.on('click', () => this.showWindow())
  }

  showWindow(navigateTo = null) {
    if (this.onShowWindow) {
      this.onShowWindow(navigateTo)
      return
    }
    const win = this.getWindow()
    if (!win) return
    if (win.isMinimized()) win.restore()
    win.show()
    win.focus()
    if (navigateTo) {
      const send = () => {
        if (!win.isDestroyed()) win.webContents.send('navigate', navigateTo)
      }
      if (win.webContents.isLoading()) win.webContents.once('did-finish-load', send)
      else send()
    }
  }

  updateWallpaperState(paused) {
    this.wallpaperPaused = !!paused
    this.rebuildMenu()
  }

  setRuntimeAvailable(available) {
    this.runtimeAvailable = !!available
    this.rebuildMenu()
  }

  rebuildMenu() {
    if (!this.tray) return
    const wallpaperSubmenu = this.runtimeAvailable
      ? [
          {
            label: this.wallpaperPaused ? 'Resume' : 'Pause',
            click: () => {
              if (this.wallpaperPaused) this.onResumeWallpaper()
              else this.onPauseWallpaper()
            },
          },
        ]
      : [{ label: 'Wallpaper unavailable', enabled: false }]
    let loginChecked = false
    try {
      loginChecked = this.isLoginItemEnabled ? !!this.isLoginItemEnabled() : false
    } catch {}
    const template = [
      { label: 'Open Paiint', click: () => this.showWindow() },
      { type: 'separator' },
      {
        label: 'Wallpaper',
        submenu: wallpaperSubmenu,
      },
      { type: 'separator' },
      {
        label: 'Settings',
        click: () => this.showWindow('Settings'),
      },
      { type: 'separator' },
      {
        label: 'Launch at login',
        type: 'checkbox',
        checked: loginChecked,
        click: (item) => {
          if (this.onToggleLoginItem) this.onToggleLoginItem(item.checked)
          this.rebuildMenu()
        },
      },
      { label: 'Exit Paiint', click: () => this.onQuit() },
    ]
    this.menu = Menu.buildFromTemplate(template)
    this.tray.setContextMenu(this.menu)
  }

  destroy() {
    if (this.tray) {
      this.tray.destroy()
      this.tray = null
      this.menu = null
    }
  }
}

module.exports = { TrayManager }
