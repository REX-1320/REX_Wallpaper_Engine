const { app, BrowserWindow, shell, ipcMain } = require('electron')
const path = require('node:path')
const fs = require('node:fs')
const { WallpaperRuntime } = require('./wallpaperRuntime.cjs')
const { TrayManager } = require('./tray-manager.cjs')

// ─── Single-instance protection ─────────────────────────────────────────────
// REX is a background app: one instance, one tray icon, one runtime. A second
// launch forwards a "restore window" command to the first instance and exits.
const gotLock = app.requestSingleInstanceLock()
if (!gotLock) app.quit()

let mainWindow = null
let isQuitting = false
let trayManager = null
const startInBackground = process.argv.includes('--background')

app.on('second-instance', () => {
  if (!gotLock) return
  if (!mainWindow) return
  if (mainWindow.isMinimized()) mainWindow.restore()
  mainWindow.show()
  mainWindow.focus()
  logLine('second-instance: restoring window')
})

const IMAGE_EXT = /\.(bmp|gif|jpe?g|png|webp|tiff?)$/i
const MODE_BY_STYLE = {
  stretched: 'stretch',
  center: 'center',
  cover: 'fill',
  fill: 'fill',
  tile: 'tile',
  fit: 'fit',
}

function logLine(...args) {
  try {
    fs.appendFileSync(path.join(app.getPath('temp'), 'rex-app.log'), `[${new Date().toISOString()}] ${args.join(' ')}\n`)
  } catch {}
}

function getWallpaperCliPath() {
  const packaged = process.resourcesPath
    ? path.join(process.resourcesPath, 'bin', 'rex-wallpaper-runtime.exe')
    : null
  if (packaged && fs.existsSync(packaged)) return packaged
  const devPath = path.join(__dirname, '..', '..', 'rex-core', '.build', 'Release', 'rex-wallpaper-runtime.exe')
  if (fs.existsSync(devPath)) return devPath
  logLine('wallpaper:runtime binary not found at', packaged || devPath)
  return packaged || devPath
}

// ─── Persistent Wallpaper Runtime ──────────────────────────────────────────
// Spawned once, kept alive, driven by LOAD/PLAY/SET_* commands. The engine
// stays warm (D3D12 + MediaFoundation), so applies are fast scene swaps
// instead of process restarts.

const runtime = new WallpaperRuntime(getWallpaperCliPath(), logLine)

let capabilities = null

// ─── Runtime crash recovery ──────────────────────────────────────────────────
// The native engine is the only thing that can repair its own desktop layer,
// so this never re-applies from Electron while the runtime is alive. If the
// runtime process itself dies (crash / killed), the app restarts it, waits for
// READY, and replays the last applied wallpaper so the desktop is restored
// without user intervention. lastApplied is only set after a successful apply.

let lastApplied = null // { kind: 'image'|'video', params }

function setRuntimeAvailable(available) {
  trayManager?.setRuntimeAvailable(available)
}

async function reapplyLastWallpaper() {
  if (!lastApplied) return
  const { kind, params } = lastApplied
  try {
    await runtime.init(params.fps ?? 60, true)
    if (kind === 'image') {
      await runtime.send('LOAD', { path: params.path, type: 'image', mode: params.mode ?? 'fill' })
    } else {
      await runtime.send('LOAD', {
        path: params.path,
        type: 'video',
        mode: params.mode ?? 'fill',
        loop: params.loop ?? true,
        muted: params.muted ?? true,
        volume: params.volume ?? 1,
        speed: params.speed ?? 1,
      })
    }
    capabilities = null
    await runtime.send('PLAY', {})
    logLine('runtime:recovery reapplied last wallpaper', kind, params.path)
    return true
  } catch (err) {
    logLine('runtime:recovery reapply ERROR', err.message)
    return false
  }
}

async function recoverRuntime() {
  if (isQuitting || !lastApplied) return
  setRuntimeAvailable(false)
  logLine('runtime:recovery start (last wallpaper queued)')
  let attempts = 0
  while (!isQuitting && attempts < 5) {
    attempts++
    try {
      if (await reapplyLastWallpaper()) {
        setRuntimeAvailable(true)
        logLine('runtime:recovery ok (attempt', attempts + ')')
        return
      }
    } catch (err) {
      logLine('runtime:recovery ERROR', err.message)
    }
    await new Promise((r) => setTimeout(r, 3000))
  }
  setRuntimeAvailable(false)
  logLine('runtime:recovery gave up after', attempts, 'attempts')
}

async function getCapabilities() {
  if (capabilities) return capabilities
  try {
    const caps = await runtime.send('GET_CAPABILITIES')
    capabilities = caps ?? { renderer: 'none', properties: [] }
  } catch (err) {
    logLine('wallpaper:capabilities ERROR', err.message)
    capabilities = { renderer: 'none', properties: [] }
  }
  return capabilities
}

async function sendIfSupported(cmd, params, applied, skipped) {
  const caps = await getCapabilities()
  const property = { SET_FILL_MODE: 'fillMode', SET_VOLUME: 'volume', SET_SPEED: 'speed', SET_LOOP: 'loop' }[cmd]
  if (!property || !caps.properties.includes(property)) {
    skipped.push(cmd)
    return
  }
  try {
    await runtime.send(cmd, params)
    applied.push(cmd)
  } catch (err) {
    logLine(cmd, 'failed:', err.message)
    skipped.push(cmd)
  }
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 640,
    title: 'REX',
    backgroundColor: '#000000',
    frame: false,
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  })
  mainWindow = win

  win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))

  win.on('maximize', () => win.webContents.send('window:maximized', true))
  win.on('unmaximize', () => win.webContents.send('window:maximized', false))

  // Close → hide, never quit. REX lives in the tray; only Exit REX quits.
  win.on('close', (event) => {
    if (!isQuitting) {
      event.preventDefault()
      win.hide()
      logLine('window: hidden (close)')
    }
  })
  win.on('closed', () => {
    if (mainWindow === win) mainWindow = null
  })

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })

  return win
}

// ─── Wallpaper pause/resume (tray) ──────────────────────────────────────────
// Mapped to existing native commands: PAUSE pauses the active scene (runtime
// emits PAUSED), PLAY resumes it (emits PLAYING). State is synced back to the
// tray menu from the runtime event stream.

async function pauseWallpaper() {
  if (!runtime.proc) return { ok: false, error: 'runtime not running' }
  try {
    await runtime.send('PAUSE', {})
    logLine('wallpaper:pause ok')
    trayManager?.updateWallpaperState(true)
    return { ok: true }
  } catch (err) {
    logLine('wallpaper:pause ERROR', err.message)
    return { ok: false, error: err.message }
  }
}

async function resumeWallpaper() {
  if (!runtime.proc) return { ok: false, error: 'runtime not running' }
  try {
    await runtime.send('PLAY', {})
    logLine('wallpaper:resume ok')
    trayManager?.updateWallpaperState(false)
    return { ok: true }
  } catch (err) {
    logLine('wallpaper:resume ERROR', err.message)
    return { ok: false, error: err.message }
  }
}

// Single shutdown path: tray exit, second-instance conflict, OS shutdown all
// converge here. Idempotent so repeated calls are harmless.
function quitApp() {
  if (isQuitting) return
  isQuitting = true
  trayManager?.destroy()
  trayManager = null
  if (mainWindow) mainWindow.destroy()
  app.quit()
}

app.whenReady().then(() => {
  if (!gotLock) return

  const tempDir = app.getPath('temp')
  logLine(
    'diagnostics:',
    'runtime.log=' + path.join(tempDir, 'REX', 'runtime.log'),
    'crash.log=' + path.join(tempDir, 'REX', 'crash.log'),
    'workerw_diag.log=' + path.join(tempDir, 'REX', 'workerw_diag.log'),
    'rex-app.log=' + path.join(tempDir, 'rex-app.log')
  )

  runtime.on('event', (event, data) => {
    if (event === 'PAUSED') trayManager?.updateWallpaperState(true)
    else if (event === 'PLAYING') trayManager?.updateWallpaperState(false)
    for (const win of BrowserWindow.getAllWindows()) {
      win.webContents.send('wallpaper:event', { event, data })
    }
  })

  ipcMain.handle('wallpaper:set', async (event, { path: filePath, style } = {}) => {
    logLine('wallpaper:set', JSON.stringify({ path: filePath, style }))
    if (typeof filePath !== 'string' || !IMAGE_EXT.test(filePath)) {
      return { ok: false, error: 'unsupported file type' }
    }
    const mode = MODE_BY_STYLE[style] ?? 'fill'
    try {
      await runtime.init()
      await runtime.send('LOAD', { path: filePath, type: 'image', mode })
      capabilities = null // adapter changed — refresh on next query
      await runtime.send('PLAY', {})
      lastApplied = { kind: 'image', params: { path: filePath, mode } }
      setRuntimeAvailable(true)
      return { ok: true, style: mode }
    } catch (err) {
      logLine('wallpaper:set ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  ipcMain.handle('wallpaper:video', async (event, { path: filePath, mode, fps, loop, muted, volume, speed } = {}) => {
    logLine('wallpaper:video', JSON.stringify({ path: filePath, mode, fps, loop, muted, volume, speed }))

    if (typeof filePath !== 'string' || !filePath) {
      lastApplied = null
      if (runtime.proc) {
        try {
          await runtime.send('STOP', {})
        } catch {
          // runtime not running — nothing to stop
        }
      }
      return { ok: true, stopped: true }
    }

    try {
      await runtime.init(fps ?? 60, true)
      await runtime.send('LOAD', {
        path: filePath,
        type: 'video',
        mode: mode ?? 'fill',
        loop: loop ?? true,
        muted: muted ?? true,
        volume: volume ?? 1,
        speed: speed ?? 1,
      })
      capabilities = null // adapter changed — refresh on next query
      await runtime.send('PLAY', {})
      lastApplied = {
        kind: 'video',
        params: { path: filePath, mode, loop, muted, volume, speed, fps },
      }
      setRuntimeAvailable(true)
      return { ok: true }
    } catch (err) {
      logLine('wallpaper:video ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  // Live property updates — only the properties the active renderer
  // actually supports are sent; the rest are reported as skipped.
  ipcMain.handle('wallpaper:update', async (event, { mode, loop, volume, speed } = {}) => {
    const applied = []
    const skipped = []
    await sendIfSupported('SET_FILL_MODE', { value: mode }, applied, skipped)
    if (loop != null) await sendIfSupported('SET_LOOP', { value: !!loop }, applied, skipped)
    if (speed != null) await sendIfSupported('SET_SPEED', { value: speed }, applied, skipped)
    if (volume != null) await sendIfSupported('SET_VOLUME', { value: volume }, applied, skipped)
    return { ok: true, applied, skipped }
  })

  ipcMain.handle('wallpaper:capabilities', async () => {
    return getCapabilities()
  })

  // Tray-driven wallpaper controls — routed to the native runtime via the
  // persistent client (PAUSE/PLAY), never direct engine access.
  ipcMain.handle('wallpaper:pause', () => pauseWallpaper())
  ipcMain.handle('wallpaper:resume', () => resumeWallpaper())

  ipcMain.handle('shell:show-in-folder', (event, { path: filePath } = {}) => {
    if (typeof filePath === 'string' && filePath) shell.showItemInFolder(filePath)
    return { ok: true }
  })

  // ─── Cached gallery thumbnails ────────────────────────────────────────────
  // Small ~480px WebP posters persisted to the REX cache at import time; the
  // gallery only ever reads these files (no video decoding in Chromium).
  const thumbDir = () => path.join(app.getPath('userData'), 'thumbnails')

  ipcMain.handle('wallpaper:save-thumbnail', (event, { id, dataUrl } = {}) => {
    try {
      if (typeof id !== 'string' || typeof dataUrl !== 'string') return { ok: false }
      const dir = thumbDir()
      fs.mkdirSync(dir, { recursive: true })
      const safeId = id.replace(/[^a-zA-Z0-9_-]/g, '_')
      const file = path.join(dir, `${safeId}.webp`)
      const base64 = dataUrl.replace(/^data:image\/\w+;base64,/, '')
      fs.writeFileSync(file, Buffer.from(base64, 'base64'))
      return { ok: true, path: file }
    } catch (err) {
      logLine('wallpaper:save-thumbnail ERROR', err.message)
      return { ok: false }
    }
  })

  ipcMain.handle('wallpaper:delete-thumbnail', (event, { path: thumbPath } = {}) => {
    try {
      if (typeof thumbPath === 'string' && thumbPath.startsWith(thumbDir())) {
        fs.rmSync(thumbPath, { force: true })
      }
    } catch {}
    return { ok: true }
  })

  // ─── Custom title bar (traffic lights) ────────────────────────────────────
  ipcMain.on('window:minimize', (event) => {
    BrowserWindow.fromWebContents(event.sender)?.minimize()
  })
  ipcMain.on('window:toggle-maximize', (event) => {
    const win = BrowserWindow.fromWebContents(event.sender)
    if (!win) return
    if (win.isMaximized()) win.unmaximize()
    else win.maximize()
  })
  ipcMain.on('window:close', (event) => {
    BrowserWindow.fromWebContents(event.sender)?.close()
  })
  ipcMain.handle('window:is-maximized', (event) => {
    return BrowserWindow.fromWebContents(event.sender)?.isMaximized() ?? false
  })

  createWindow()

  trayManager = new TrayManager({
    getWindow: () => mainWindow,
    onQuit: () => quitApp(),
    onPauseWallpaper: () => pauseWallpaper(),
    onResumeWallpaper: () => resumeWallpaper(),
  })
  trayManager.create()
  logLine('tray: created')

  // Runtime crash → recovery. The native engine repairs its own layer; this
  // only restarts the process and replays the last wallpaper. Ignored while
  // quitting (will-quit calls runtime.stop() → exit fires harmlessly).
  runtime.on('exit', (code) => {
    logLine('runtime:exit', code, '(recovering)')
    recoverRuntime()
  })

  // Normal launch shows the UI; --background (e.g. Windows startup) keeps the
  // window hidden — tray + native services only.
  if (!startInBackground && mainWindow) {
    mainWindow.show()
    logLine('window: shown (normal launch)')
  } else {
    logLine('window: kept hidden (background launch)')
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

// REX is a background application — the tray owns the lifetime. Closing the
// window hides it (see the close handler above); we never quit on
// window-all-closed. Only Exit REX (quitApp) terminates the app.
app.on('window-all-closed', () => {})

app.on('will-quit', () => {
  runtime.stop()
})
