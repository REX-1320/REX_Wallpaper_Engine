const { app, BrowserWindow, shell, ipcMain, screen } = require('electron')
const path = require('node:path')
const fs = require('node:fs')
const { WallpaperRuntime } = require('./wallpaperRuntime.cjs')
const { TrayManager } = require('./tray-manager.cjs')
const { Supervisor } = require('./supervisor/supervisor.cjs')
const { AppStateManager } = require('./app-state-manager.cjs')
const { WallpaperLibrary } = require('./wallpaper-library.cjs')
const startup = require('./startup.cjs')
const { isPathInside } = require('./path-guard.cjs')
const { resolveWindowBounds } = require('./window-bounds.cjs')
const { generatePoster } = require('./poster-worker.cjs')
const { probeMediaFile } = require('./mediaInfo.cjs')
const { PlaylistScheduler } = require('./playlist-scheduler.cjs')
const { migrateUserData } = require('./userdata-migrate.cjs')
const { cleanupLegacyLoginItem } = require('./login-item-migrate.cjs')

// ─── Identity ────────────────────────────────────────────────────────────────
// Pin userData BEFORE any consumer (including the thumbnail worker below).
// Electron derives it from the app name, so a rename would otherwise silently
// move it and orphan every library, playlist and setting. The migration is
// idempotent and never deletes the old profile.
const userDataMigration = migrateUserData(app.getPath('appData'))
app.setPath('userData', userDataMigration.target)
app.setAppUserModelId('com.paiint.desktop')
if (userDataMigration.source) {
  const errors = userDataMigration.errors.length
    ? ` (errors: ${userDataMigration.errors.join('; ')})`
    : ''
  console.log(
    `[migrate] copied ${userDataMigration.copied} item(s) from ${userDataMigration.source}${errors}`,
  )
}

// ─── GPU thumbnail worker mode ────────────────────────────────────────────────
// A separate, hardware-accelerated Electron process that decodes one poster and
// exits (see thumb-worker.cjs). It must be handled BEFORE
// disableHardwareAcceleration() below, and must never touch the single-instance
// lock, tray, runtime or window lifecycle that follow.
if (process.argv.includes('--paiint-thumb-worker')) {
  require('./thumb-worker.cjs')
    .run(process.argv)
    .then((code) => app.exit(code || 0))
    .catch(() => app.exit(1))
  return
}

// The Electron UI is a settings surface; the wallpaper itself is rendered by
// the separate native runtime (D3D12). Software-compositing the UI keeps its
// GPU footprint near zero dedicated VRAM while the app is parked in the tray.
// Must run before the app is ready. The runtime process is unaffected.
app.disableHardwareAcceleration()

// ─── Single-instance protection ─────────────────────────────────────────────
// Paiint is a background app: one instance, one tray icon, one runtime. A second
// launch forwards a "restore window" command to the first instance and exits.
const gotLock = app.requestSingleInstanceLock()
if (!gotLock) {
  // Another instance owns the single-instance lock - this one has nothing to do.
  logLine('app: quit', 'another instance holds the lock')
  app.quit()
}

// Windows keeps the old login entry under the app's previous name. Remove it
// once so signing in cannot start the retired build alongside this one.
if (gotLock) {
  cleanupLegacyLoginItem({ log: (line) => console.log(line) })
}

let mainWindow = null
let isQuitting = false
let trayManager = null
const startInBackground = process.argv.includes('--background')

// Window lifecycle state. The BrowserWindow is destroyed on close and recreated
// on demand; nothing may assume mainWindow exists.
let navigateReady = false // renderer registered its onNavigate listener
let pendingNavigate = null // destination queued until that listener is ready
let revealTimer = null
let revealWin = null
const closingWindows = new WeakSet()
// The playlist scheduler lives HERE, not in the renderer (see
// playlist-scheduler.cjs). The renderer mirrors a rotation plan (membership +
// order + settings) and main owns the Timer and native scene-end triggers, so
// the window can be destroyed on close and a --background login start can
// rotate with no renderer at all.
let playlistScheduler = null

// Dev-aware login-item registration: packaged apps register the exe
// directly, but dev Electron needs the app folder as argv[1] or Windows
// boots a default Electron window instead of Paiint.
// "Start minimised" (the default) adds --background: the login item then
// starts the tray and the wallpaper without opening the window.
function applyLoginItem(enabled) {
  const startMinimized = appState?.get('startup.startMinimized') !== false
  const settings = app.isPackaged
    ? startup.loginItemSettings(enabled, undefined, { startMinimized })
    : startup.loginItemSettings(enabled, app.getAppPath(), { startMinimized })
  app.setLoginItemSettings(settings)
}

// Is the login item actually registered? The rule lives in startup.js (pure,
// tested): getLoginItemSettings().openAtLogin is unreliable on Windows - in a
// dev build it reads FALSE even though the entry was written, because it is
// derived from an identity-keyed registry entry. launchItems is the truth.
function loginItemEnabled() {
  try {
    return startup.loginItemEnabled(app.getLoginItemSettings())
  } catch {
    return false
  }
}

// The engine's INIT knobs the app owns. maxFps is the presentation cap
// (0 = uncapped) and hardwareAcceleration is the HW-decode switch; both are
// read when the runtime process starts.
function engineInitOptions() {
  const maxFps = Number(appState?.get('engine.maxFps') ?? 0)
  return {
    maxFps: Number.isFinite(maxFps) ? Math.min(240, Math.max(0, maxFps)) : 0,
    hardwareAcceleration: appState?.get('engine.hardwareAcceleration') !== false,
  }
}

app.on('second-instance', () => {
  if (!gotLock) return
  logLine('second-instance: restoring window')
  showMainWindow()
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
    const logPath = path.join(app.getPath('temp'), 'paiint-app.log')
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

// Window-interaction diagnostics. Off by default; set PAIINT_WINDBG=1 to trace
// maximize-state and drag size-drift while reproducing titlebar bugs.
function winDbg(...args) {
  if (process.env.PAIINT_WINDBG) logLine('[win]', ...args)
}

function getWallpaperCliPath() {
  const packaged = process.resourcesPath
    ? path.join(process.resourcesPath, 'bin', 'paiint-wallpaper-runtime.exe')
    : null
  if (packaged && fs.existsSync(packaged)) return packaged
  const devPath = path.join(__dirname, '..', '..', 'rex-core', '.build', 'Release', 'paiint-wallpaper-runtime.exe')
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

// Main-initiated renderer liveness probe (Supervisor._rendererTick). A page
// timer can be throttled while the window is still "visible" (fully occluded
// by another always-on-top window), so a quiet heartbeat alone is not proof of
// a hang. The supervisor asks the renderer over IPC before reloading it; main
// process -> renderer IPC is not subject to page-timer throttling, so a reply
// proves the main thread is alive. Tokens stop a late reply from satisfying a
// newer probe.
let rendererProbeSeq = 0
const pendingRendererProbes = new Map()

function probeRenderer(timeoutMs = 2500) {
  const win = mainWindow
  if (!win || win.isDestroyed() || win.webContents.isDestroyed()) return Promise.resolve(false)
  const token = ++rendererProbeSeq
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      pendingRendererProbes.delete(token)
      resolve(false)
    }, timeoutMs)
    pendingRendererProbes.set(token, () => {
      clearTimeout(timer)
      pendingRendererProbes.delete(token)
      resolve(true)
    })
    try {
      win.webContents.send('renderer:probe', token)
    } catch {
      clearTimeout(timer)
      pendingRendererProbes.delete(token)
      resolve(false)
    }
  })
}

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
    // Images are OS-drawn: INIT would build the 4K D3D12 engine for a
    // wallpaper that never uses it (measured 84 MB private + ~103 MB VRAM).
    // Only a video needs the engine — the native video LOAD rejects a null
    // engine, while the image LOAD is explicitly engine-free.
    if (kind === 'image') {
      await runtime.send('LOAD', { path: params.path, type: 'image', mode: params.mode ?? 'fill' })
    } else {
      const init = engineInitOptions()
      await runtime.init(init.maxFps, init.hardwareAcceleration)
      await runtime.send('LOAD', {
        path: params.path,
        type: 'video',
        mode: params.mode ?? 'fill',
        loop: params.loop ?? true,
        muted: params.muted ?? true,
        volume: params.volume ?? 1,
        speed: params.speed ?? 1,
      })
      if (params.audioMode != null) {
        try {
          await runtime.send('SET_AUDIO_MODE', { value: params.audioMode })
        } catch (err) {
          logLine('runtime:recovery SET_AUDIO_MODE failed', err.message)
        }
      }
    }
    capabilities = null
    // Images have no engine scene: PLAY always failed with "No scene
    // loaded" and would mark a successful recovery as failed.
    if (kind !== 'image') {
      await runtime.send('PLAY', {})
    }
    logLine('runtime:recovery reapplied last wallpaper', kind, params.path)
    return true
  } catch (err) {
    logLine('runtime:recovery reapply ERROR', err.message)
    return false
  }
}

// ─── Idle engine reclaim ────────────────────────────────────────────────────
// A static image is drawn by the OS. The native engine (D3D12 device, video
// pool) is dead weight while one is showing — and the AMD driver pins every
// imported shared-texture pool until the process exits (measured: ~180 MiB
// per 4K video session; releasing imports, trimming and destroying both
// devices does not return it). The process is therefore the reclaim
// boundary: an image apply deliberately stops the runtime. The next video
// apply lazily spawns a fresh one (cold INIT ≈ 0.7 s, once per video).
function reclaimIdleRuntime(reason) {
  if (!runtime.proc) return
  // Image-only runtime (never INITed): it is ~10 MB with no engine and no
  // VRAM, and it keeps image LOADs / fill-mode updates warm. Keep it.
  if (!runtime.inited) return
  logLine('runtime:reclaim', reason)
  // intentional → the supervisor must not treat this exit as a crash and
  // replay the wallpaper (replaying would respawn the runtime).
  runtime.stop({ intentional: true })
  capabilities = null
}

// Live updates used to change only the running engine; a crash then
// replayed the STALE apply-time settings from startup's last-wallpaper.json
// (audit 5.10 #5). Merge successful live updates into lastApplied and
// persist them — debounced so slider drags don't hammer the disk.
let _persistTimer = null
// Last SET_* summary logged, so a slider drag does not write a line per pixel.
let lastUpdateSummary = ''
let lastUpdateValues = ''
let lastUpdateLogAt = 0
function persistLastAppliedDebounced() {
  if (_persistTimer) clearTimeout(_persistTimer)
  _persistTimer = setTimeout(() => {
    _persistTimer = null
    if (lastApplied) startup.saveLastApplied(app.getPath('userData'), lastApplied)
  }, 500)
  if (_persistTimer.unref) _persistTimer.unref()
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
  const property = { SET_FILL_MODE: 'fillMode', SET_VOLUME: 'volume', SET_SPEED: 'speed', SET_LOOP: 'loop', SET_MUTE: 'mute', SET_AUDIO_MODE: 'audioMode', SET_BRIGHTNESS: 'brightness', SET_CONTRAST: 'contrast', SET_SATURATION: 'saturation' }[cmd]
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
      // No INIT: this is the OS wallpaper path, not an engine scene (see the
      // native LOAD branch). The image is applied either way; skipping INIT
      // keeps an image-only runtime at ~10 MB private and 0 MB VRAM instead
      // of building a 4K D3D12 engine for it.
      await runtime.send('LOAD', { path: params.path, type: 'image', mode: params.mode ?? 'fill' })
    } else {
      const init = engineInitOptions()
      await runtime.init(init.maxFps, init.hardwareAcceleration)
      await runtime.send('LOAD', {
        path: params.path,
        type: 'video',
        mode: params.mode ?? 'fill',
        loop: params.loop ?? true,
        muted: params.muted ?? true,
        volume: params.volume ?? 1,
        speed: params.speed ?? 1,
      })
      if (params.audioMode != null) {
        try {
          await runtime.send('SET_AUDIO_MODE', { value: params.audioMode })
        } catch (err) {
          logLine('startup: re-apply SET_AUDIO_MODE failed', err.message)
        }
      }
      // Colour adjustments are not part of LOAD either — the boot replay has
      // to restore them or the wallpaper comes back at the engine's default.
      const restored = []
      for (const [cmd, value] of [['SET_BRIGHTNESS', params.brightness], ['SET_CONTRAST', params.contrast], ['SET_SATURATION', params.saturation]]) {
        if (value == null) continue
        try {
          await runtime.send(cmd, { value })
          restored.push(`${cmd}=${value}`)
        } catch (err) {
          logLine(`startup: re-apply ${cmd} failed`, err.message)
        }
      }
      if (restored.length > 0) logLine('startup: re-applied', restored.join(' '))
    }
    capabilities = null // adapter changed — refresh on next query
    // Images are applied through the OS wallpaper API (see the LOAD branch),
    // not an engine scene. PLAY would always fail with "No scene loaded" and
    // incorrectly mark a successful boot restore as failed.
    if (kind !== 'image') {
      await runtime.send('PLAY', {})
    }
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

// Apply one image through the OS wallpaper path. Shared by the wallpaper IPC
// handler and the main-process playlist scheduler so a rotation uses exactly
// the same LOAD sequence (and the same lastApplied/persistence bookkeeping) as
// a user apply.
async function applyImageEntry({ path: filePath, mode }, source = 'wallpaper:set') {
  if (typeof filePath !== 'string' || !IMAGE_EXT.test(filePath)) {
    throw new Error('unsupported file type')
  }
  const engineMode = mode ?? 'fill'
  // No INIT: LOAD applies the image through the OS wallpaper API and is valid
  // with no engine (the runtime spawns on demand). INIT would build a 4K D3D12
  // engine the image never uses - measured 84 MB private and ~103 MB VRAM.
  await runtime.send('LOAD', { path: filePath, type: 'image', mode: engineMode })
  capabilities = null // adapter changed - refresh on next query
  lastApplied = { kind: 'image', params: { path: filePath, mode: engineMode } }
  startup.saveLastApplied(app.getPath('userData'), lastApplied)
  if (supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
  setRuntimeAvailable(true)
  // The image is on the desktop now: free the engine instead of leaving it
  // warm for a video that may never come.
  reclaimIdleRuntime(`${source} image applied`)
  logLine(`${source}: image applied`, filePath)
  return { ok: true, style: engineMode }
}

// Apply one video scene. Same contract as applyImageEntry; mirrors the
// wallpaper:video handler (audio layout and colour are live SET_* commands,
// not part of LOAD).
async function applyVideoEntry(params, source = 'wallpaper:video') {
  const { path: filePath, mode, fps, loop, muted, volume, speed, audioMode, brightness, contrast, saturation } = params
  if (typeof filePath !== 'string' || !filePath) throw new Error('invalid video path')
  const init = engineInitOptions()
  await runtime.init(init.maxFps, init.hardwareAcceleration)
  await runtime.send('LOAD', {
    path: filePath,
    type: 'video',
    mode: mode ?? 'fill',
    loop: loop ?? true,
    muted: muted ?? true,
    volume: volume ?? 1,
    speed: speed ?? 1,
  })
  if (audioMode != null) {
    // Non-fatal: the wallpaper must apply even if the layout request is
    // refused (the engine falls back and reports it).
    try {
      await runtime.send('SET_AUDIO_MODE', { value: audioMode })
    } catch (err) {
      logLine(`${source} SET_AUDIO_MODE failed`, err.message)
    }
  }
  // A scene starts at 1.0/1.0/1.0, so without these a wallpaper switch would
  // inherit the previous file's brightness. Non-fatal, same as above.
  for (const [cmd, value] of [['SET_BRIGHTNESS', brightness], ['SET_CONTRAST', contrast], ['SET_SATURATION', saturation]]) {
    if (value == null) continue
    try {
      await runtime.send(cmd, { value })
    } catch (err) {
      logLine(`${source} ${cmd} failed`, err.message)
    }
  }
  capabilities = null // adapter changed - refresh on next query
  await runtime.send('PLAY', {})
  lastApplied = {
    kind: 'video',
    params: { path: filePath, mode, loop, muted, volume, speed, fps, audioMode, brightness, contrast, saturation },
  }
  startup.saveLastApplied(app.getPath('userData'), lastApplied)
  if (supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
  setRuntimeAvailable(true)
  logLine(`${source}: video applied`, filePath)
  return { ok: true }
}

function createWindow() {
  // Remember where the user put the window -- but only restore those bounds
  // if they are still on a display (a monitor may have been unplugged).
  const savedBounds = appState?.get('window') ?? null
  const restored = resolveWindowBounds(savedBounds, screen.getAllDisplays(), {
    width: 1280,
    height: 800,
    minWidth: 960,
    minHeight: 640,
  })

  const win = new BrowserWindow({
    width: restored?.width ?? 1280,
    height: restored?.height ?? 800,
    ...(restored ? { x: restored.x, y: restored.y } : {}),
    minWidth: 960,
    minHeight: 640,
    title: 'Paiint',
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
      // Keep renderer timers at full rate when the window is occluded/hidden.
      // Chromium throttles background pages, and after ~5 minutes it drops
      // timers to about once a minute. The 2 s supervisor heartbeat lives in a
      // page timer, so throttling made the watchdog reload a healthy renderer
      // ("renderer stale 16s - reloading" every ~75 s) while the window was
      // covered by another app. Playlist timers benefit from the same setting.
      backgroundThrottling: false,
    },
  })
  mainWindow = win
  navigateReady = false

  // PAIINT_WINDBG=1: log the OS move-loop boundaries so a drag that never starts
  // (or starts then aborts) can be told apart instead of guessed at.
  // ENTERSIZEMOVE = OS modal move loop started. EXITSIZEMOVE = it ended.
  // CAPTURECHANGED = mouse capture lost (aborts a drag).
  if (process.env.PAIINT_WINDBG && typeof win.hookWindowMessage === 'function') {
    const WM = { LBUTTONDOWN: 0x201, LBUTTONUP: 0x202, CAPTURECHANGED: 0x215, ENTERSIZEMOVE: 0x231, EXITSIZEMOVE: 0x232, MOVING: 0x216 }
    for (const [name, id] of Object.entries(WM)) {
      try {
        win.hookWindowMessage(id, () => { winDbg('msg=' + name) })
      } catch {}
    }
  }

  win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))

  // Remember the window's size and position. Debounced: a drag fires a
  // stream of move/resize events, and each one would otherwise be a disk
  // write. The maximized state is deliberately NOT stored -- restoring a
  // maximized window would open the app full-screen unasked.
  let boundsTimer = null
  const rememberBounds = () => {
    if (appState?.get('ui.rememberWindowBounds') === false) return
    if (win.isDestroyed() || win.isMinimized() || win.isMaximized() || win.isFullScreen()) return
    if (boundsTimer) clearTimeout(boundsTimer)
    boundsTimer = setTimeout(() => {
      boundsTimer = null
      try {
        const b = win.getNormalBounds()
        appState?.set('window', { x: b.x, y: b.y, width: b.width, height: b.height })
      } catch {}
    }, 400)
    if (boundsTimer.unref) boundsTimer.unref()
  }
  win.on('resize', rememberBounds)
  win.on('move', rememberBounds)

  win.on('maximize', () => {
    winDbg('event=maximize nativeIsMaximized=', win.isMaximized(), 'bounds=', JSON.stringify(win.getBounds()))
    win.webContents.send('window:maximized', true)
  })
  win.on('unmaximize', () => {
    winDbg('event=unmaximize nativeIsMaximized=', win.isMaximized(), 'bounds=', JSON.stringify(win.getBounds()))
    win.webContents.send('window:maximized', false)
  })

  // Close → tear the renderer down. Paiint lives in the tray, and the native
  // runtime is a separate process that keeps the wallpaper playing. Destroying
  // (rather than hiding) is what actually releases the renderer and its GPU
  // allocations; a hidden window otherwise keeps both for the app's lifetime.
  //
  // A running playlist is NOT a reason to keep the renderer alive any more:
  // its Timer/scene-end scheduler is in main (playlist-scheduler.cjs), so the
  // rotation continues in the tray.
  //
  // "Quit Paiint on close" is the user's choice (tray.closeToTray === false);
  // the default keeps the app alive in the tray.
  win.on('close', (event) => {
    if (isQuitting) return
    event.preventDefault()
    if (appState?.get('tray.closeToTray') === false) {
      logLine('window: close -> quit (tray.closeToTray = false)')
      quitApp('window-close')
      return
    }
    win.hide()
    closingWindows.add(win)
    logLine('window: destroying (close)')
    // destroy() skips beforeunload/unload, so essential main-process state is
    // flushed here, synchronously, while the renderer still exists.
    try { appState?.flush() } catch {}
    setImmediate(() => {
      if (!win.isDestroyed()) win.destroy()
    })
  })
  win.on('closed', () => {
    // A newer window may already have replaced this one (e.g. second-instance
    // during teardown). Only clear state that still belongs to this window.
    if (mainWindow !== win) return
    mainWindow = null
    navigateReady = false
    pendingNavigate = null
    if (revealWin === win) {
      if (revealTimer) clearTimeout(revealTimer)
      revealTimer = null
      revealWin = null
    }
    logLine('window: destroyed')
  })
  win.webContents.on('did-start-loading', () => { navigateReady = false })

  // ─── Renderer watchdog bridge (audit 5.10 #9) ─────────────────────────────
  // A crashed renderer used to leave a dead window forever: report it through
  // the supervisor and reload. Liveness pings arrive via 'renderer:ping'
  // (preload heartbeat); show/restore re-baseline the staleness clock because
  // hidden/minimized renderers have their timers throttled.
  win.webContents.on('render-process-gone', (_event, details) => {
    supervisor?.handleRendererGone(details)
  })
  win.on('show', () => supervisor?.recordRendererPing())
  win.on('restore', () => supervisor?.recordRendererPing())

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })

  return win
}

// ─── Window lifecycle: one window, created on demand ───────────────────────
// Close destroys the window (see createWindow). Every reopen path — tray
// click, tray Settings, second-instance, app activate — funnels through
// showMainWindow so concurrent requests can never create two windows.
function flushPendingNavigate(win) {
  if (!pendingNavigate || !navigateReady) return
  if (!win || win.isDestroyed() || win.webContents.isDestroyed()) return
  if (closingWindows.has(win)) return
  const page = pendingNavigate
  pendingNavigate = null
  win.webContents.send('navigate', page)
  logLine('window: navigate', page)
}

function revealWindow(win) {
  if (revealWin === win) {
    if (revealTimer) clearTimeout(revealTimer)
    revealTimer = null
    revealWin = null
  }
  if (!win || win.isDestroyed() || closingWindows.has(win)) return
  if (!win.isVisible()) win.show()
  win.focus()
}

function scheduleReveal(win) {
  // A newer window supersedes any reveal still pending for an older one.
  if (revealWin === win && revealTimer) return
  if (revealTimer) clearTimeout(revealTimer)
  revealWin = win
  win.once('ready-to-show', () => revealWindow(win))
  // Never leave a created window permanently hidden if ready-to-show is missed.
  revealTimer = setTimeout(() => revealWindow(win), 2000)
}

function showMainWindow(navigateTo = null) {
  if (navigateTo) pendingNavigate = navigateTo

  // second-instance can fire before ready in a very tight relaunch.
  if (!app.isReady()) {
    app.once('ready', () => showMainWindow())
    return null
  }

  let win = mainWindow
  if (!win || win.isDestroyed() || closingWindows.has(win)) {
    win = createWindow()
  }

  if (win.isMinimized()) win.restore()
  if (win.isVisible()) {
    win.focus()
    flushPendingNavigate(win)
    return win
  }

  if (win.webContents.isLoading()) {
    scheduleReveal(win)
  } else {
    revealWindow(win)
  }
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
function quitApp(reason = 'requested') {
  if (isQuitting) return
  isQuitting = true
  // Without this the app could disappear with no trace in the log, which made
  // every exit look like a mystery crash.
  logLine('app: quit', reason)
  try { appState?.flush() } catch {}
  trayManager?.destroy()
  trayManager = null
  if (revealTimer) clearTimeout(revealTimer)
  revealTimer = null
  revealWin = null
  pendingNavigate = null
  if (mainWindow) mainWindow.destroy()
  app.quit()
}

app.whenReady().then(() => {
  if (!gotLock) return

  const tempDir = app.getPath('temp')
  logLine(
    'diagnostics:',
    'runtime.log=' + path.join(tempDir, 'Paiint', 'runtime.log'),
    'crash.log=' + path.join(tempDir, 'Paiint', 'crash.log'),
    'workerw_diag.log=' + path.join(tempDir, 'Paiint', 'workerw_diag.log'),
    'paiint-app.log=' + path.join(tempDir, 'paiint-app.log')
  )

  runtime.on('event', (event, data) => {
    if (event === 'PAUSED') trayManager?.updateWallpaperState(true)
    else if (event === 'PLAYING') trayManager?.updateWallpaperState(false)
    // Scene end is a rotation trigger for main now (the UI may not exist).
    if (event === 'ENDED' && playlistScheduler) {
      playlistScheduler.handleEvent(event, data).catch((err) => logLine('playlist: ENDED handler ERROR', err.message))
    }
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
      return await applyImageEntry({ path: filePath, mode }, 'wallpaper:set')
    } catch (err) {
      logLine('wallpaper:set ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  ipcMain.handle('wallpaper:video', async (event, { path: filePath, mode, fps, loop, muted, volume, speed, audioMode, brightness, contrast, saturation } = {}) => {
    logLine('wallpaper:video', JSON.stringify({ path: filePath, mode, fps, loop, muted, volume, speed, audioMode, brightness, contrast, saturation }))

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
      // No wallpaper scene left: the engine has nothing to draw.
      reclaimIdleRuntime('wallpaper cleared')
      return { ok: true, stopped: true }
    }

    try {
      await applyVideoEntry({ path: filePath, mode, fps, loop, muted, volume, speed, audioMode, brightness, contrast, saturation }, 'wallpaper:video')
      return { ok: true }
    } catch (err) {
      logLine('wallpaper:video ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  // Live property updates — only the properties the active renderer
  // actually supports are sent; the rest are reported as skipped.
  ipcMain.handle('wallpaper:update', async (event, { mode, loop, volume, speed, muted, audioMode, brightness, contrast, saturation } = {}) => {
    const applied = []
    const skipped = []
    // SET_FILL_MODE is the only update without a null guard — sending it with
    // an undefined value reset the fill mode every time any other property
    // was updated.
    //
    // Images are applied through the OS wallpaper API, not an engine scene:
    // the image-only runtime reports no renderer properties, so the engine
    // SET_FILL_MODE below was silently skipped and the style never changed.
    // Re-issue the image LOAD with the new mode instead — the native image
    // branch is engine-free, unloads any stale scene, and is idempotent.
    if (mode != null && lastApplied?.kind === 'image' && lastApplied.params?.path) {
      try {
        await runtime.send('LOAD', {
          path: lastApplied.params.path,
          type: 'image',
          mode,
        })
        // Reuse the SET_FILL_MODE bookkeeping so mergeLiveUpdate persists the
        // new mode in last-wallpaper.json (same field as a video).
        applied.push('SET_FILL_MODE')
      } catch (err) {
        skipped.push('SET_FILL_MODE')
        logLine('wallpaper:update image mode ERROR', err.message)
      }
      // Normally a no-op (the image apply already reclaimed the engine); it
      // covers a mode change issued while a video engine is still warm.
      reclaimIdleRuntime('image mode updated')
    } else if (mode != null) {
      await sendIfSupported('SET_FILL_MODE', { value: mode }, applied, skipped)
    }
    if (loop != null) await sendIfSupported('SET_LOOP', { value: !!loop }, applied, skipped)
    if (speed != null) await sendIfSupported('SET_SPEED', { value: speed }, applied, skipped)
    if (volume != null) await sendIfSupported('SET_VOLUME', { value: volume }, applied, skipped)
    if (muted != null) await sendIfSupported('SET_MUTE', { value: !!muted }, applied, skipped)
    if (audioMode != null) await sendIfSupported('SET_AUDIO_MODE', { value: audioMode }, applied, skipped)
    // Colour adjustments: live, per frame, 0..2 with 1.0 = unchanged.
    if (brightness != null) await sendIfSupported('SET_BRIGHTNESS', { value: brightness }, applied, skipped)
    if (contrast != null) await sendIfSupported('SET_CONTRAST', { value: contrast }, applied, skipped)
    if (saturation != null) await sendIfSupported('SET_SATURATION', { value: saturation }, applied, skipped)
    // Persist only what actually applied: a skipped property must not
    // enter the crash-replay state (replaying it would resurrect a value
    // the renderer refuses, or one the user never saw accepted).
    if (applied.length > 0 && lastApplied) {
      if (startup.mergeLiveUpdate(lastApplied, applied, { mode, loop, volume, speed, muted, audioMode, brightness, contrast, saturation })) {
        persistLastAppliedDebounced()
      }
    }
    // Log what the engine actually took: immediately when the SET of commands
    // changes, and at most once a second while the values keep changing (a
    // slider drag fires one update per pixel, and the log is read by humans).
    const summary = `applied=${applied.join(',') || '-'} skipped=${skipped.join(',') || '-'}`
    const valueSummary = `${summary} ${JSON.stringify({ mode, loop, volume, speed, muted, audioMode, brightness, contrast, saturation })}`
    const now = Date.now()
    if (summary !== lastUpdateSummary || (valueSummary !== lastUpdateValues && now - lastUpdateLogAt > 1000)) {
      lastUpdateSummary = summary
      lastUpdateValues = valueSummary
      lastUpdateLogAt = now
      logLine('wallpaper:update', valueSummary)
    }
    return { ok: true, applied, skipped }
  })

  ipcMain.handle('wallpaper:capabilities', async () => {
    return getCapabilities()
  })

  // "Does this file have an audio track?" The container header answers it
  // without decoding: the inspector hides the audio controls for silent
  // files instead of offering mute/volume for a track that does not exist.
  ipcMain.handle('wallpaper:media-info', async (event, { path: filePath } = {}) => {
    if (typeof filePath !== 'string' || !filePath) return { container: null, hasAudio: null }
    return probeMediaFile(filePath)
  })

  // The wallpaper the runtime is actually showing (main re-applies it at boot
  // from last-wallpaper.json). The renderer adopts it instead of pretending
  // nothing is applied until the user clicks Apply again.
  ipcMain.handle('wallpaper:last-applied', () => ({
    kind: lastApplied?.kind ?? null,
    path: lastApplied?.params?.path ?? null,
  }))

  // Transport controls (Inspector + tray). Pause/resume used to be tray-only:
  // the renderer had no IPC surface because nothing in the UI called it. The
  // Inspector now does, so expose the two existing commands.
  ipcMain.handle('wallpaper:pause', () => pauseWallpaper())
  ipcMain.handle('wallpaper:resume', () => resumeWallpaper())

  // The engine's own telemetry, for the Settings page. Never spawn a runtime
  // to answer a question: the panel must not start the engine (and its VRAM)
  // just because the user opened the page.
  ipcMain.handle('wallpaper:status', async () => {
    if (!runtime.proc) return { available: false, reason: 'runtime-stopped' }
    try {
      const data = await runtime.send('GET_STATUS', {}, 5000)
      if (!data) return { available: false, reason: 'no-answer' }
      // What the engine can apply right now (the live adapter's property
      // list). Read from the same in-memory answer - it must not spawn a
      // runtime, and the runtime is already there.
      let capabilities = null
      try {
        const caps = await runtime.send('GET_CAPABILITIES', {}, 5000)
        capabilities = caps?.properties ?? null
      } catch {}
      return { available: true, data, capabilities }
    } catch (err) {
      logLine('wallpaper:status ERROR', err.message)
      return { available: false, reason: 'error' }
    }
  })

  // Renderer liveness heartbeat for the supervisor watchdog (audit 5.10 #9):
  // preload sends one every 2 s; only a visible-but-stale renderer is treated
  // as hung (Supervisor._rendererTick).
  ipcMain.on('renderer:ping', () => supervisor?.recordRendererPing())

  // Reply to the supervisor's main-initiated liveness probe. A reply is also a
  // heartbeat: it proves the renderer's main thread is responsive even when
  // its page timers are throttled. The sender check keeps other windows from
  // satisfying a probe aimed at the app window.
  ipcMain.on('renderer:probe-reply', (event, token) => {
    if (!mainWindow || mainWindow.isDestroyed()) return
    if (event.sender !== mainWindow.webContents) return
    supervisor?.recordRendererPing()
    pendingRendererProbes.get(token)?.()
  })

  // Tray "Settings" may target a window that was just created. The preload
  // announces when the renderer's onNavigate listener is actually mounted;
  // only then is it safe to deliver a queued destination.
  ipcMain.on('renderer:navigate-ready', (event) => {
    if (!mainWindow || mainWindow.isDestroyed()) return
    if (event.sender !== mainWindow.webContents) return
    navigateReady = true
    flushPendingNavigate(mainWindow)
  })

  // Tray-driven pause/resume and the launch-at-login toggle have no IPC
  // surface anymore: their only caller was the dead preload API removed in
  // audit 5.10 #15. The tray drives both via TrayManager callbacks (below).

  ipcMain.handle('shell:show-in-folder', (event, { path: filePath } = {}) => {
    if (typeof filePath === 'string' && filePath) shell.showItemInFolder(filePath)
    return { ok: true }
  })

  // ─── Cached gallery thumbnails ────────────────────────────────────────────
  // Small ~480px WebP posters persisted to the Paiint cache at import time; the
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
      // Containment with separator + case-insensitive compare (audit 5.10
      // #14): a bare startsWith let sibling dirs ('...\thumbnails-evil') and
      // case-variant paths through.
      if (isPathInside(thumbPath, thumbDir())) {
        fs.rmSync(thumbPath, { force: true })
      }
    } catch {}
    return { ok: true }
  })

  // HEVC posters: the UI runs without hardware acceleration, which removes
  // Chromium's HEVC decoder, so the renderer's <video> cannot produce a frame.
  // Delegate to the short-lived GPU worker instead (it exits immediately after
  // writing one WebP, so the parked UI keeps its 0 MB VRAM footprint).
  ipcMain.handle('wallpaper:fallback-thumbnail', async (event, payload = {}) => {
    const filePath = typeof payload.path === 'string' ? payload.path : ''
    const maxSize = Number.isFinite(payload.maxSize) ? Math.min(960, Math.max(64, payload.maxSize)) : 480
    if (!filePath || !path.isAbsolute(filePath)) return { ok: false, reason: 'invalid-path' }
    try {
      if (!fs.statSync(filePath).isFile()) return { ok: false, reason: 'not-a-file' }
    } catch {
      return { ok: false, reason: 'missing' }
    }
    const result = await generatePoster(filePath, maxSize)
    if (!result.ok) logLine('wallpaper:fallback-thumbnail failed', path.basename(filePath), result.reason)
    return result
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

  // ─── App-level settings (the Settings page) ───────────────────────────────

  // The launch-at-login item. The OS is the source of truth here (the tray
  // checkbox reads the same state), so the page and the tray can never
  // disagree.
  ipcMain.handle('app:login-item', (event, payload = {}) => {
    try {
      const enabled = typeof payload?.enabled === 'boolean' ? payload.enabled : null
      if (enabled != null) {
        applyLoginItem(enabled)
        logLine('settings: login item', enabled ? 'enabled' : 'disabled')
        // The tray shows the same preference; rebuild it so its checkbox
        // cannot disagree with the page.
        try { trayManager?.rebuildMenu() } catch {}
      }
      return { enabled: loginItemEnabled(), startMinimized: appState?.get('startup.startMinimized') !== false }
    } catch (err) {
      logLine('app:login-item ERROR', err.message)
      return { enabled: false, startMinimized: true }
    }
  })

  // Where Paiint keeps things, so the page can show the real paths and open
  // them. The poster cache and the log folder are the two a user actually
  // needs when something looks wrong.
  ipcMain.handle('app:paths', () => {
    const tempDir = app.getPath('temp')
    return {
      library: path.join(app.getPath('userData'), 'wallpaper-library.json'),
      posters: path.join(app.getPath('userData'), 'thumbnails'),
      logs: path.join(tempDir, 'Paiint'),
      crashes: path.join(tempDir, 'Paiint', 'crashes'),
      appLog: path.join(tempDir, 'paiint-app.log'),
    }
  })

  // Open one of Paiint's own folders in Explorer. Guarded: only paths the app
  // itself reported may be opened.
  ipcMain.handle('app:open-path', async (event, { target } = {}) => {
    const dirs = {
      library: path.dirname(path.join(app.getPath('userData'), 'wallpaper-library.json')),
      posters: path.join(app.getPath('userData'), 'thumbnails'),
      logs: path.join(app.getPath('temp'), 'Paiint'),
      crashes: path.join(app.getPath('temp'), 'Paiint', 'crashes'),
    }
    const dir = dirs[target]
    if (!dir) return { ok: false, reason: 'unknown-target' }
    try {
      await shell.openPath(dir)
      return { ok: true }
    } catch (err) {
      logLine('app:open-path ERROR', err.message)
      return { ok: false, reason: err.message }
    }
  })

  // Clear the cached poster images. The library itself is untouched - only
  // the derived thumbnails go, and they are re-created on the next visit.
  ipcMain.handle('app:clear-posters', () => {
    const dir = path.join(app.getPath('userData'), 'thumbnails')
    try {
      if (!fs.existsSync(dir)) return { ok: true, removed: 0 }
      let removed = 0
      for (const name of fs.readdirSync(dir)) {
        const file = path.join(dir, name)
        if (!fs.statSync(file).isFile()) continue
        fs.rmSync(file, { force: true })
        removed += 1
      }
      logLine('settings: cleared poster cache', removed + ' file(s)')
      return { ok: true, removed }
    } catch (err) {
      logLine('app:clear-posters ERROR', err.message)
      return { ok: false, reason: err.message }
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
  // The renderer mirrors a compact rotation plan (active playlist, ordered
  // queue, live settings). Main schedules Timer + native scene-end from it, so
  // the rotation survives the window being destroyed and a --background start.
  // The invoke returns the authoritative countdown/state for the dialog.
  ipcMain.handle('playlist:sync-plan', (event, plan) => {
    return playlistScheduler ? playlistScheduler.setPlan(plan) : { active: false }
  })
  ipcMain.handle('playlist:scheduler-state', () => {
    return playlistScheduler ? playlistScheduler.state() : { active: false }
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
  // The window is created on demand (normal launch or tray open), not here:
  // --background starts tray + native services only.

  // Persisted UI state + gallery library (P0.3). The library self-registers
  // its gallery.* IPC on construct; app-state needs the two handlers below.
  //
  // Created BEFORE the tray and the boot re-apply: both read settings from it
  // (the login item's start-minimised flag, the engine's frame cap, whether
  // the last wallpaper is restored at boot).
  appState = new AppStateManager()
  _galleryLib = new WallpaperLibrary()
  // The real build version, for the Settings page's About section.
  try { appState.set('version', app.getVersion()) } catch {}
  logLine('persistence: app-state + gallery library ready')

  // Playlist scheduling that outlives the UI. The renderer sends the plan;
  // main owns Timer and the native ENDED event, applies through the same
  // helpers the IPC handlers use, and persists enough state to resume after a
  // login start. Every side effect is injected so the rules are unit-tested
  // without Electron (playlist-scheduler.test.cjs).
  playlistScheduler = new PlaylistScheduler({
    now: Date.now,
    log: logLine,
    exists: (entry) => {
      try {
        return fs.statSync(entry.path).isFile()
      } catch {
        return false
      }
    },
    apply: async (entry) => {
      const apply = entry.apply ?? {}
      if (apply.kind === 'image') return applyImageEntry({ path: entry.path, mode: apply.mode }, 'playlist')
      if (apply.kind === 'video') return applyVideoEntry({ ...apply, path: entry.path }, 'playlist')
      throw new Error(`unsupported playlist member kind: ${apply.kind ?? 'none'}`)
    },
    emit: (event) => {
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed() && !win.webContents.isDestroyed()) win.webContents.send('playlist:applied', event)
      }
    },
    persist: (snapshot) => appState.set('playlistEngine.rotation', snapshot),
  })
  logLine('playlist: main-process scheduler ready')

  trayManager = new TrayManager({
    getWindow: () => mainWindow,
    onShowWindow: (navigateTo) => showMainWindow(navigateTo),
    onQuit: () => quitApp('tray'),
    onPauseWallpaper: () => pauseWallpaper(),
    onResumeWallpaper: () => resumeWallpaper(),
    isLoginItemEnabled: () => {
      return loginItemEnabled()
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

  // ─── Boot re-apply ────────────────────────────────────────────────────────
  // "Restore the last wallpaper at launch" is a user setting; when it is off
  // the desktop is left exactly as it was.
  const bootEntry = appState.get('startup.restoreWallpaper') === false
    ? null
    : startup.loadLastApplied(app.getPath('userData'))
  let bootApplyDone = Promise.resolve(false)
  if (bootEntry) {
    bootApplyDone = applyStoredWallpaper(bootEntry).then((ok) => {
      if (!ok) logLine('startup: re-apply failed, desktop left as-is')
      return ok
    })
  } else if (appState.get('startup.restoreWallpaper') === false) {
    logLine('startup: restore-at-launch is off, desktop left as-is')
  } else {
    logLine('startup: no persisted wallpaper, nothing to re-apply')
  }

  // ─── Launch at login ──────────────────────────────────────────────────────
  // Default-enable on FIRST RUN only and remember that we did; later launches
  // respect whatever the user chose (tray toggle writes the OS state
  // directly). Registering unconditionally re-enabled the login item on every
  // start, overriding "off" (audit 5.10 #8).
  try {
    if (!appState.get('settings.loginItemInitialized')) {
      applyLoginItem(true)
      appState.set('settings.loginItemInitialized', true)
      logLine('startup: registered for login (first run)')
    } else {
      let enabled = false
      try {
        enabled = app.getLoginItemSettings().openAtLogin
      } catch {}
      logLine('startup: login item left as configured (enabled=' + enabled + ')')
    }
  } catch (err) {
    logLine('startup: login-item setup ERROR', err.message)
  }

  // Runtime health → Supervisor (P0.2). It owns exit-driven recovery
  // (bounded reapply rounds, then park), hang/renderer watchdogs, crash
  // reports and recovery events. Replaces the old inline recoverRuntime so
  // exactly one driver replays the wallpaper (no double-reapply races).
  // The heartbeat is capability-probed: the first PING that answers arms the
  // miss counter (limit 3, ~6 s); an older runtime binary without PING stays
  // parked instead of reading as hung every interval. Renderer watchdog
  // stays silent until the first renderer ping arrives (guard inside the
  // supervisor).
  supervisor = new Supervisor({
    runtime,
    logLine,
    getWindow: () => mainWindow,
    probeRenderer,
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
    logs: {
      runtimeLog: path.join(tempDir, 'Paiint', 'runtime.log'),
      crashLog: path.join(tempDir, 'Paiint', 'crash.log'),
      workerwDiagLog: path.join(tempDir, 'Paiint', 'workerw_diag.log'),
      // Unchanged from logLine(): the Electron log lives at the temp root,
      // not under Paiint\ — the old path never existed, so reports silently
      // lacked the app log (audit 5.10 #10).
      appLog: path.join(tempDir, 'paiint-app.log'),
    },
  })
  supervisor.start()

  // ─── Resume the playlist rotation in main ────────────────────────────────
  // Must run AFTER the boot replay: an overdue Timer restored from the plan
  // would otherwise race the wallpaper main is restoring. A normal launch's
  // renderer may have already mirrored a fresher plan; in that case keep it.
  // With --background there is no renderer at all, so the persisted plan is
  // the only source - this is what makes a login start rotate.
  bootApplyDone.finally(() => {
    if (!playlistScheduler) return
    if (!playlistScheduler.state().mode) {
      const saved = appState.get('playlistEngine.rotation')
      if (saved?.plan) {
        playlistScheduler.restore(saved)
        logLine('playlist: restored rotation', saved.plan.mode, `queue=${saved.plan.queue?.length ?? 0}`)
      }
    }
    const bootPath = lastApplied?.params?.path
    if (bootPath) playlistScheduler.reconcile(bootPath)
    playlistScheduler.start()
  })

  // Normal launch opens the UI; --background (e.g. Windows startup) starts the
  // tray + native services only and defers the window to the first tray open.
  if (!startInBackground) {
    showMainWindow()
    logLine('window: shown (normal launch)')
    // PAIINT_WINDBG=1: DOM-level caption audit. Logs what actually sits at the
    // titlebar viewport points, so a dead drag region can be blamed on a
    // covering overlay instead of guessed at.
    if (process.env.PAIINT_WINDBG) {
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
    logLine('window: deferred (background launch)')
  }

  app.on('activate', () => {
    showMainWindow()
  })
})

// Paiint is a background application — the tray owns the lifetime. Closing the
// window destroys it (see the close handler) but leaves the app and the native
// runtime running; only Exit Paiint (quitApp) terminates the process.
app.on('window-all-closed', () => {})

app.on('will-quit', () => {
  if (supervisor) supervisor.stop() // quitting: exits must not trigger recovery
  if (playlistScheduler) playlistScheduler.stop() // stop timers; plan is already persisted
  // The scheduler's last change may still sit in the app-state debounce.
  try { appState?.flush() } catch {}
  runtime.stop()
})
