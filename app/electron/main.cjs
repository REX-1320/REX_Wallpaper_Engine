const { app, BrowserWindow, shell, ipcMain } = require('electron')
const path = require('node:path')
const fs = require('node:fs')
const { WallpaperRuntime } = require('./wallpaperRuntime.cjs')
const { TrayManager } = require('./tray-manager.cjs')
const { Supervisor } = require('./supervisor/supervisor.cjs')
const { AppStateManager } = require('./app-state-manager.cjs')
const { WallpaperLibrary } = require('./wallpaper-library.cjs')
const startup = require('./startup.cjs')

// ─── Single-instance protection ─────────────────────────────────────────────
// REX is a background app: one instance, one tray icon, one runtime. A second
// launch forwards a "restore window" command to the first instance and exits.
const gotLock = app.requestSingleInstanceLock()
if (!gotLock) app.quit()

let mainWindow = null
let isQuitting = false
let trayManager = null
const startInBackground = process.argv.includes('--background')

// Dev-aware login-item registration: packaged apps register the exe
// directly, but dev Electron needs the app folder as argv[1] or Windows
// boots a default Electron window instead of REX.
function applyLoginItem(enabled) {
  const settings = app.isPackaged
    ? startup.loginItemSettings(enabled)
    : startup.loginItemSettings(enabled, app.getAppPath())
  app.setLoginItemSettings(settings)
}

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

const LOG_PATH_MAX_BYTES = 2 * 1024 * 1024

function logLine(...args) {
  try {
    const logPath = path.join(app.getPath('temp'), 'rex-app.log')
    try {
      // A tray app runs for days and this logger is synchronous; never let
      // the file grow without bound. Previous log moves aside once.
      if (fs.statSync(logPath).size > LOG_PATH_MAX_BYTES) {
        fs.renameSync(logPath, logPath + '.1')
      }
    } catch {}
    fs.appendFileSync(logPath, `[${new Date().toISOString()}] ${args.join(' ')}\n`)
  } catch {}
}

// Window-interaction diagnostics. Off by default; set REX_WINDBG=1 to trace
// maximize-state and drag size-drift while reproducing titlebar bugs.
function winDbg(...args) {
  if (process.env.REX_WINDBG) logLine('[win]', ...args)
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

// Supervisor owns runtime health (P0.2): exit-driven recovery (replacing the
// old inline recoverRuntime), hang/renderer watchdogs, crash
// reports and recovery events. Assigned in app.whenReady (needs app paths).
let supervisor = null

// Persisted UI state + gallery library (P0.3). Assigned in app.whenReady:
// both constructors resolve userData paths, which need the app module ready.
// _galleryLib is reference-held only: the library self-registers its
// gallery.* IPC on construct (no further calls needed from here).
let appState = null
let _galleryLib = null

let capabilities = null

// ─── Runtime crash recovery ──────────────────────────────────────────────────
// Owned by the Supervisor (see whenReady): exit → bounded reapply rounds →
// park. lastApplied is only set after a successful apply; reapplyLastWallpaper
// is the supervisor's reapply callback. The native engine repairs its own
// desktop layer; Electron only restarts the process and replays wallpaper.

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
  const property = { SET_FILL_MODE: 'fillMode', SET_VOLUME: 'volume', SET_SPEED: 'speed', SET_LOOP: 'loop', SET_MUTE: 'mute' }[cmd]
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

// Re-applies a persisted { kind, params } entry (boot re-apply after login).
// Same LOAD/PLAY sequence as the IPC handlers below; returns true on success.
async function applyStoredWallpaper(entry) {
  if (!entry || (entry.kind !== 'image' && entry.kind !== 'video')) return false
  const { kind, params } = entry
  try {
    if (kind === 'image') {
      await runtime.init()
      await runtime.send('LOAD', { path: params.path, type: 'image', mode: params.mode ?? 'fill' })
    } else {
    await runtime.init(params.fps ?? 0, true)
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
    capabilities = null // adapter changed — refresh on next query
    await runtime.send('PLAY', {})
    lastApplied = entry
    startup.saveLastApplied(app.getPath('userData'), lastApplied)
    if (supervisor) supervisor.resetRecovery()
    setRuntimeAvailable(true)
    logLine('startup: re-applied last wallpaper', kind, params.path)
    return true
  } catch (err) {
    logLine('startup: re-apply ERROR', err.message)
    return false
  }
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 640,
    title: 'REX',
    // NOT transparent. A transparent window is not snap-assist aware
    // (electron#37888) and cannot be maximized by double-click (PR #28207), so
    // it can never behave like a normal Windows app. Instead the glass comes
    // from the system-drawn DWM material -- exactly how Windows Terminal does
    // it -- which also paints behind the non-client area.
    backgroundMaterial: 'mica',
    // Hidden NATIVE caption instead of frame:false. A frameless window has no
    // WS_CAPTION, so Windows refuses to Aero-Snap it and gives us no native
    // drag loop. With the caption hidden the OS owns the titlebar drag, the
    // snap preview and the snap itself — the same behaviour Windows Terminal
    // has — while our own traffic lights still paint over it.
    titleBarStyle: 'hidden',
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  })
  mainWindow = win

  // REX_WINDBG=1: log the OS move-loop boundaries so a drag that never starts
  // (or starts then aborts) can be told apart instead of guessed at.
  // ENTERSIZEMOVE = OS modal move loop started. EXITSIZEMOVE = it ended.
  // CAPTURECHANGED = mouse capture lost (aborts a drag).
  if (process.env.REX_WINDBG && typeof win.hookWindowMessage === 'function') {
    const WM = { LBUTTONDOWN: 0x201, LBUTTONUP: 0x202, CAPTURECHANGED: 0x215, ENTERSIZEMOVE: 0x231, EXITSIZEMOVE: 0x232, MOVING: 0x216 }
    for (const [name, id] of Object.entries(WM)) {
      try {
        win.hookWindowMessage(id, () => { winDbg('msg=' + name) })
      } catch {}
    }
  }

  win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))

  win.on('maximize', () => {
    winDbg('event=maximize nativeIsMaximized=', win.isMaximized(), 'bounds=', JSON.stringify(win.getBounds()))
    win.webContents.send('window:maximized', true)
  })
  win.on('unmaximize', () => {
    winDbg('event=unmaximize nativeIsMaximized=', win.isMaximized(), 'bounds=', JSON.stringify(win.getBounds()))
    win.webContents.send('window:maximized', false)
  })

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
      // No scene exists after an image LOAD (OS-drawn wallpaper), so PLAY
      // is expected to fail with "No scene loaded" — the image is applied
      // regardless. Never fail the apply (or skip persistence) for that.
      try {
        await runtime.send('PLAY', {})
      } catch (playErr) {
        logLine('wallpaper:set PLAY-after-image (expected)', playErr.message)
      }
      lastApplied = { kind: 'image', params: { path: filePath, mode } }
      startup.saveLastApplied(app.getPath('userData'), lastApplied)
      if (supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
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
      startup.clearLastApplied(app.getPath('userData'))
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
      await runtime.init(fps ?? 0, true)
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
      startup.saveLastApplied(app.getPath('userData'), lastApplied)
      if (supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
      setRuntimeAvailable(true)
      return { ok: true }
    } catch (err) {
      logLine('wallpaper:video ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  // Live property updates — only the properties the active renderer
  // actually supports are sent; the rest are reported as skipped.
  ipcMain.handle('wallpaper:update', async (event, { mode, loop, volume, speed, muted } = {}) => {
    const applied = []
    const skipped = []
    // SET_FILL_MODE is the only update without a null guard — sending it with
    // an undefined value reset the fill mode every time any other property
    // was updated.
    if (mode != null) await sendIfSupported('SET_FILL_MODE', { value: mode }, applied, skipped)
    if (loop != null) await sendIfSupported('SET_LOOP', { value: !!loop }, applied, skipped)
    if (speed != null) await sendIfSupported('SET_SPEED', { value: speed }, applied, skipped)
    if (volume != null) await sendIfSupported('SET_VOLUME', { value: volume }, applied, skipped)
    if (muted != null) await sendIfSupported('SET_MUTE', { value: !!muted }, applied, skipped)
    return { ok: true, applied, skipped }
  })

  ipcMain.handle('wallpaper:capabilities', async () => {
    return getCapabilities()
  })

  // Tray-driven wallpaper controls — routed to the native runtime via the
  // persistent client (PAUSE/PLAY), never direct engine access.
  ipcMain.handle('wallpaper:pause', () => pauseWallpaper())
  ipcMain.handle('wallpaper:resume', () => resumeWallpaper())

  // Launch-at-login toggle (tray menu + future settings UI).
  ipcMain.handle('settings:get-login-item', () => {
    try {
      return { ok: true, enabled: app.getLoginItemSettings().openAtLogin }
    } catch (err) {
      return { ok: false, error: err.message }
    }
  })
  ipcMain.handle('settings:set-login-item', (event, { enabled } = {}) => {
    try {
      applyLoginItem(enabled)
      logLine('settings: login item', enabled ? 'enabled' : 'disabled')
      trayManager?.rebuildMenu?.()
      return { ok: true, enabled: !!enabled }
    } catch (err) {
      return { ok: false, error: err.message }
    }
  })

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

  // ─── Persisted UI state (P0.3) ────────────────────────────────────────────
  // Backs the renderer's useAppState hook. Dot-path get/set straight through
  // to AppStateManager (versioned file + backups + corruption recovery).
  ipcMain.handle('app-state:get', (event, keyPath) => {
    try {
      if (typeof keyPath !== 'string' || !appState) return null
      return appState.get(keyPath) ?? null
    } catch (err) {
      logLine('app-state:get ERROR', err.message)
      return null
    }
  })
  ipcMain.handle('app-state:set', (event, keyPath, value) => {
    try {
      if (typeof keyPath !== 'string' || !appState) return null
      return appState.set(keyPath, value) ?? null
    } catch (err) {
      logLine('app-state:set ERROR', err.message)
      return null
    }
  })

  // ─── Gallery file restore (P0.3) ──────────────────────────────────────────
  // Reads a persisted library path back into a data URL so the gallery can
  // rebuild File objects after a restart. Missing/unreadable files report
  // ok:false ('not-found') and the gallery skips them (kept in the library,
  // never silently pruned — the drive may be temporarily unavailable).
  ipcMain.handle('gallery.read-file', (event, { path: filePath } = {}) => {
    try {
      if (typeof filePath !== 'string' || !filePath) return { ok: false, error: 'bad-path' }
      const stat = fs.statSync(filePath)
      if (!stat.isFile()) return { ok: false, error: 'not-found' }
      if (stat.size > 150 * 1024 * 1024) return { ok: false, error: 'too-large' }
      const ext = path.extname(filePath).toLowerCase()
      const mime = {
        '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
        '.webp': 'image/webp', '.gif': 'image/gif', '.bmp': 'image/bmp',
        '.mp4': 'video/mp4', '.webm': 'video/webm', '.mov': 'video/quicktime',
      }[ext] ?? 'application/octet-stream'
      const dataUrl = `data:${mime};base64,${fs.readFileSync(filePath).toString('base64')}`
      return { ok: true, dataUrl, name: path.basename(filePath), size: stat.size, lastModified: stat.mtimeMs }
    } catch (err) {
      return { ok: false, error: err.code === 'ENOENT' ? 'not-found' : err.message }
    }
  })

  // ─── Custom title bar (traffic lights) ────────────────────────────────────
  ipcMain.on('window:minimize', (event) => {
    BrowserWindow.fromWebContents(event.sender)?.minimize()
  })
  ipcMain.on('window:toggle-maximize', (event) => {
    const win = BrowserWindow.fromWebContents(event.sender)
    if (!win) return
    const before = win.isMaximized()
    winDbg('toggle: isMaximized(before)=', before, 'bounds=', JSON.stringify(win.getBounds()))
    if (before) win.unmaximize()
    else win.maximize()
    winDbg('toggle: isMaximized(after)=', win.isMaximized(), 'bounds=', JSON.stringify(win.getBounds()))
  })
  ipcMain.on('window:close', (event) => {
    BrowserWindow.fromWebContents(event.sender)?.close()
  })
  ipcMain.handle('window:is-maximized', (event) => {
    const v = BrowserWindow.fromWebContents(event.sender)?.isMaximized() ?? false
    winDbg('is-maximized ->', v)
    return v
  })

  // ─── Window move / maximize / Aero Snap ─────────────────────────────────────
  // Owned entirely by the OS. The titlebar strip is a real
  // -webkit-app-region: drag region and the window is NOT transparent, so
  // Windows runs its own caption (HTCAPTION) modal move loop. That gives drag,
  // double-click maximize/restore, Aero Snap (halves, quarters, corners),
  // drag-from-maximized, the snap preview and the system menu -- byte-for-byte
  // the same behaviour as Windows Terminal, because it is the same code path.
  //
  // A hand-rolled poll-loop implementation lived here and could not match it:
  //   * a 	ransparent: true window is not snap-assist aware (electron#37888)
  //     and cannot be maximized by double-clicking (PR #28207);
  //   * it snapped the OUTER rect to the work-area half, so the client came out
  //     SMALLER than half -- native instead inflates the rect by the invisible
  //     resize border (Terminal reports -9,0 1938x2169 for a 1920x2160 client);
  //   * its size pin ratcheted up ~1px on every drag (1280 -> 1281 -> 1282).
  createWindow()

  trayManager = new TrayManager({
    getWindow: () => mainWindow,
    onQuit: () => quitApp(),
    onPauseWallpaper: () => pauseWallpaper(),
    onResumeWallpaper: () => resumeWallpaper(),
    isLoginItemEnabled: () => {
      try {
        return app.getLoginItemSettings().openAtLogin
      } catch {
        return false
      }
    },
    onToggleLoginItem: (enabled) => {
      try {
        applyLoginItem(enabled)
        logLine('settings: login item', enabled ? 'enabled' : 'disabled')
      } catch (err) {
        logLine('settings: login item ERROR', err.message)
      }
    },
  })
  trayManager.create()
  logLine('tray: created')

  // ─── Launch at login + boot re-apply ──────────────────────────────────────
  // Register every launch (idempotent): REX is a resident wallpaper app and
  // must survive reboots. The toggle lives in the tray menu + settings IPC.
  try {
    applyLoginItem(true)
    logLine('startup: registered for login (--background)')
  } catch (err) {
    logLine('startup: registration ERROR', err.message)
  }
  const bootEntry = startup.loadLastApplied(app.getPath('userData'))
  if (bootEntry) {
    applyStoredWallpaper(bootEntry).then((ok) => {
      if (!ok) logLine('startup: re-apply failed, desktop left as-is')
    })
  } else {
    logLine('startup: no persisted wallpaper, nothing to re-apply')
  }

  // Persisted UI state + gallery library (P0.3). The library self-registers
  // its gallery.* IPC on construct; app-state needs the two handlers below.
  appState = new AppStateManager()
  _galleryLib = new WallpaperLibrary()
  logLine('persistence: app-state + gallery library ready')

  // Runtime health → Supervisor (P0.2). It owns exit-driven recovery
  // (bounded reapply rounds, then park), hang/renderer watchdogs, crash
  // reports and recovery events. Replaces the old inline recoverRuntime so
  // exactly one driver replays the wallpaper (no double-reapply races).
  // Heartbeat is parked (miss limit Infinity) until the native PING command
  // lands in a linked binary — without it every healthy runtime would read
  // as hung. Renderer watchdog stays silent until the first renderer ping
  // arrives (guard inside the supervisor).
  supervisor = new Supervisor({
    runtime,
    logLine,
    getWindow: () => mainWindow,
    getLastApplied: () => lastApplied,
    reapply: async () => {
      if (!lastApplied || isQuitting) return true // nothing to do: not a failure
      return reapplyLastWallpaper()
    },
    setRuntimeAvailable,
    onEvent: (msg) => {
      logLine('supervisor:event', msg.event)
      for (const win of BrowserWindow.getAllWindows()) {
        win.webContents.send('wallpaper:event', msg)
      }
    },
    stateFilePath: path.join(app.getPath('userData'), 'supervisor-state.json'),
    logs: {
      runtimeLog: path.join(tempDir, 'REX', 'runtime.log'),
      crashLog: path.join(tempDir, 'REX', 'crash.log'),
      workerwDiagLog: path.join(tempDir, 'REX', 'workerw_diag.log'),
      appLog: path.join(tempDir, 'REX', 'rex-app.log'),
    },
    options: { heartbeatMissLimit: Number.POSITIVE_INFINITY },
  })
  supervisor.start()

  // Normal launch shows the UI; --background (e.g. Windows startup) keeps the
  // window hidden — tray + native services only.
  if (!startInBackground && mainWindow) {
    mainWindow.show()
    logLine('window: shown (normal launch)')
    // REX_WINDBG=1: DOM-level caption audit. Logs what actually sits at the
    // titlebar viewport points, so a dead drag region can be blamed on a
    // covering overlay instead of guessed at.
    if (process.env.REX_WINDBG) {
      setTimeout(() => {
        if (!mainWindow || mainWindow.isDestroyed()) return
        mainWindow.webContents.executeJavaScript(`(() => {
          const out = []
          const cx = Math.round(window.innerWidth * 0.5)
          const qx = Math.round(window.innerWidth * 0.25)
          for (const [nx, x] of [['50%', cx], ['25%', qx]]) {
            const el = document.elementFromPoint(x, 24)
            out.push(nx + ' -> ' + (el ? (el.tagName + '.' + el.className + '#' + el.id) : 'null'))
          }
          const strip = document.querySelector('.titlebar-drag')
          out.push('strip-present=' + !!strip)
          if (strip) {
            const cs = getComputedStyle(strip)
            out.push('strip-region=' + cs['-webkit-app-region'] + ' rect=' + JSON.stringify(strip.getBoundingClientRect()))
          }
          out.push('dialogs=' + document.querySelectorAll('.pl-dialog-overlay,.pl-dialog,.playlist-context-menu').length)
          out.push('fixed-full=' + [...document.querySelectorAll('*')].filter(e => {
            const r = e.getBoundingClientRect()
            const cs = getComputedStyle(e)
            return cs.position === 'fixed' && r.width >= window.innerWidth - 2 && r.height >= 44 && r.top <= 0
          }).map(e => e.tagName + '.' + e.className).join('|'))
          return out.join(' ;; ')
        })()`).then(
          (s) => logLine('[win] dom-audit:', s),
          (e) => logLine('[win] dom-audit ERROR', String(e && e.message || e))
        )
      }, 4000)
    }
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
  if (supervisor) supervisor.stop() // quitting: exits must not trigger recovery
  runtime.stop()
})
