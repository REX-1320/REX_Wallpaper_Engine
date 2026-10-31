const { app, BrowserWindow, shell, ipcMain, screen, powerMonitor } = require('electron')
const path = require('node:path')
const fs = require('node:fs')
const { spawn } = require('node:child_process')
const os = require('node:os')
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
const { desktopLayerReady, mergeRecoveryEntry } = require('./recovery.cjs')
const { migrateUserData } = require('./userdata-migrate.cjs')
const { cleanupLegacyLoginItem } = require('./login-item-migrate.cjs')
const startupTask = require('./startup-task.cjs')
const { legalDocPath } = require('./legal-docs.cjs')
const { isWindowOpen, effectiveMute, desktopCovered: decideDesktopCovered } = require('./audio-visibility.cjs')
const { parseOcclusionLine } = require('./occlusion.cjs')
const { DiagnosticsRecorder } = require('./diagnostics/recorder.cjs')
const { collectDeviceInfo } = require('./diagnostics/device-info.cjs')

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
// Login launches: the portable Run key passes --background. The MSIX startup
// task launches with no arguments, so its stub passes --startup-task instead;
// whether the window opens then depends on startup.startMinimized, which is
// only readable after app-state loads.
const startupTaskLaunch = process.argv.includes('--startup-task')

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
//
// The packaged build uses the manifest's windows.startupTask instead: the
// helper's path is where that stub ships. It reports "unpackaged" in the
// portable build, where the Run-key path below is used.
function startupTaskStubPath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'bin', 'paiint-startup.exe')
  }
  return path.join(__dirname, '..', '..', 'rex-core', 'build', 'Release', 'paiint-startup.exe')
}

// null = no package identity (portable): the caller must use the Run key.
// Any string = the package's StartupTask state (enabled, disabled-by-user, ...).
//
// The helper is a child process, so the answer is cached: the tray rebuilds
// its menu on every wallpaper state change and must not block on a spawn.
// Every write below refreshes the cache, so a Settings toggle is visible at
// once; Task Manager changes are picked up on the next refresh.
let startupTaskCache = { at: 0, state: null }
function startupTaskState() {
  const now = Date.now()
  if (startupTaskCache.at && now - startupTaskCache.at < 10000) {
    return startupTaskCache.state
  }
  const stub = startupTaskStubPath()
  const state = startupTask.query(stub)
  if (state === startupTask.UNAVAILABLE) {
    // The stub is on disk but produced no state (blocked, timed out, crashed).
    // Falling back keeps the app usable, but say so: in a package the Run key
    // is virtualized and this fallback cannot work.
    logLine('startup: task helper gave no state, falling back to the Run key')
    startupTaskCache = { at: now, state: null }
    return null
  }
  startupTaskCache = { at: now, state }
  return state
}

function applyLoginItem(enabled) {
  // Packaged: the manifest's windows.startupTask is the only registration
  // Windows honours. app.setLoginItemSettings writes HKCU Run, which MSIX
  // redirects into the package hive (the P1 "launch at login is a no-op").
  if (startupTaskState() !== null) {
    const next = startupTask.setEnabled(startupTaskStubPath(), enabled)
    startupTaskCache = { at: Date.now(), state: next }
    logLine('startup: task', enabled ? 'enable' : 'disable', '->', next)
    return
  }

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
// The packaged build asks the startup-task helper instead: Task Manager can
// disable the task, and that state is invisible to the Run key.
function loginItemEnabled() {
  const taskState = startupTaskState()
  if (taskState !== null) return startupTask.isEnabled(taskState)
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
  // Dev: the canonical CMake tree is rex-core/build (AGENTS.md build command).
  // .build is a legacy tree; it held an older runtime without the quit
  // snapshot, so a source launch silently tested yesterday's engine. Prefer
  // build, fall back to .build only when it is the one that exists.
  const devCandidates = [
    path.join(__dirname, '..', '..', 'rex-core', 'build', 'Release', 'paiint-wallpaper-runtime.exe'),
    path.join(__dirname, '..', '..', 'rex-core', '.build', 'Release', 'paiint-wallpaper-runtime.exe'),
  ]
  const devPath = devCandidates.find((candidate) => fs.existsSync(candidate)) ?? devCandidates[0]
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

let lastApplied = null

// Occlusion-based audio mute: mute the wallpaper when the desktop is covered
// by any window (not just a Paiint window). Driven by a background monitor.
let desktopOccluded = false
let occlusionDetail = ''
let occlusionReported = false
let occlusionMonitor = null
let occlusionRestartTimer = null
let occlusionRestartDelay = 1000

// Local-only test-mode recorder (Settings > Advanced, or --diagnostics).
let diagnostics = null
let diagnosticsStart = null
let diagnosticsForced = false
// Last root actually resolved. Kept after stop so the status row and Open
// button still point at the folder the archived session went to.
let diagnosticsRootInfo = null

function setRuntimeAvailable(available) {
  trayManager?.setRuntimeAvailable(available)
}

// Merge the gallery library's saved per-file settings (the inspector writes
// them) over a persisted { kind, params } entry. The two files are written on
// different debounces, so after a crash either can be newer; the library is
// the durable per-file record and wins when it has an entry.
function hydrateStoredEntry(entry) {
  if (!entry?.params?.path) return entry
  try {
    const stored = _galleryLib?.get?.(entry.params.path)
    return mergeRecoveryEntry(entry, stored?.settings)
  } catch {
    return entry
  }
}

// The desktop layer is verified, not assumed. The supervisor may only report a
// successful recovery once the native engine actually owns the scene: images
// are proven by the LOAD response itself (SetImageWallpaper succeeded), while
// videos must report a running or paused scene with a decoded duration.
async function verifyVideoDesktop(timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs
  let last = null
  while (Date.now() < deadline) {
    try {
      last = await runtime.send('GET_STATUS', {}, 1500)
      if (desktopLayerReady(last)) return true
    } catch (err) {
      last = err.message
    }
    await new Promise((resolve) => setTimeout(resolve, 250))
  }
  logLine('runtime: desktop layer not verified', JSON.stringify(last))
  return false
}

// Shared by the boot replay and crash recovery. Returns true only when the
// wallpaper is actually back on the desktop.
async function replayStoredEntry(entry, source, opts = {}) {
  if (!entry || (entry.kind !== 'image' && entry.kind !== 'video')) return false
  const hydrated = hydrateStoredEntry(entry)
  if (hydrated.kind === 'image') {
    await applyImageEntry(hydrated.params, source, opts)
    return true
  }
  await applyVideoEntry(hydrated.params, source, opts)
  return verifyVideoDesktop()
}

async function reapplyLastWallpaper() {
  // Only what THIS session applied is recovered. A wallpaper restored at boot
  // is already in lastApplied (applyImageEntry/applyVideoEntry set it), and a
  // user who turned restore-at-launch off must not have an old file replayed
  // by a crash.
  if (!lastApplied) return true // nothing to do: not a failure
  try {
    const ok = await replayStoredEntry(lastApplied, 'runtime:recovery', { resetRecovery: false })
    if (!ok) return false
    const path = lastApplied.params?.path ?? null
    // A plan that points at another file must not keep rotating over the
    // scene we just verified.
    if (path && playlistScheduler) playlistScheduler.reconcile(path)
    logLine('runtime:recovery desktop layer verified', path)
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

// ─── Window-aware wallpaper audio ───────────────────────────────────────────
// The per-wallpaper `muted` value is the user's intent. The Settings override
// can only add mute while a Paiint window is on screen; it never changes what
// the user chose. lastApplied.params.muted therefore always stores intent.
function audioOnlyWhenClosed() {
  return appState?.get('engine.audioOnlyWhenWindowClosed') === true
}

function videoIntentMuted() {
  if (lastApplied?.kind !== 'video') return null
  // Older persisted entries predate the field; the engine default is muted.
  return lastApplied.params?.muted !== false
}

function effectiveVideoMute(intentMuted) {
  return effectiveMute({
    // Callers pass a boolean (videoIntentMuted() normalises old entries to
    // true); an accidental null must not read as "muted".
    intentMuted: intentMuted === true,
    onlyWhenClosed: audioOnlyWhenClosed(),
    windowOpen: desktopCovered(),
  })
}

// Until the monitor has reported once, the Paiint window is the only signal we
// trust (mute, the safe default). After that its enumeration is authoritative:
// it already counts the Paiint window when it covers the desktop, and it knows
// when the desktop is genuinely visible again (Win+D raises Progman above every
// window, including ours, while isWindowOpen() still reports true because the
// window was never minimized).
function desktopCovered() {
  return decideDesktopCovered({
    occlusionReported,
    desktopOccluded,
    windowOpen: isWindowOpen(mainWindow),
  })
}

// The whole mute decision in one object: the sample collector and the SET_MUTE
// path must never disagree about why the wallpaper is silent.
function audioDecision() {
  const intentMuted = videoIntentMuted()
  return {
    intentMuted,
    onlyWhenClosed: audioOnlyWhenClosed(),
    windowOpen: isWindowOpen(mainWindow),
    desktopOccluded,
    occlusionReported,
    muted: effectiveVideoMute(intentMuted),
  }
}

// Only log a decision when it changes: show/hide/minimize fire in bursts and
// every SET_MUTE must stay auditable without flooding the session.
let lastAudioDecision = ''

async function applyEffectiveMute(source) {
  if (isQuitting) return { ok: true, skipped: 'quitting' }
  if (lastApplied?.kind !== 'video') return { ok: true, skipped: 'no-video' }
  const decision = audioDecision()
  const muted = decision.muted
  const key = [
    decision.intentMuted,
    decision.onlyWhenClosed,
    decision.windowOpen,
    decision.desktopOccluded,
    decision.occlusionReported,
    muted,
  ].join('|')
  if (key !== lastAudioDecision) {
    lastAudioDecision = key
    diagnostics?.event('audio', { source, ...decision })
  }
  if (!runtime.proc) return { ok: true, skipped: 'runtime-stopped', muted }
  try {
    await runtime.send('SET_MUTE', { value: muted })
    logLine('audio: effective mute', source, muted ? 'muted' : 'audible')
    return { ok: true, muted }
  } catch (err) {
    logLine('audio: SET_MUTE failed', source, err.message)
    return { ok: false, error: err.message }
  }
}

// ─── Occlusion-based audio mute ─────────────────────────────────────────────
// Mute the wallpaper when the desktop is covered by any window (not just a
// Paiint window). A background PowerShell process enumerates the top-level
// windows and reports the state only when it changes.
//
// Foreground-only detection was wrong: clicking the desktop makes Progman the
// foreground window while every other window stays visible, so the wallpaper
// spoke through them. Enumeration also sees unfocused and second-monitor
// windows.
//
// Ignored: the desktop itself (Progman/WorkerW), the taskbar, minimized,
// invisible and DWM-cloaked windows (other virtual desktops / suspended UWP),
// and windows covering less than 0.5 % of a monitor. That last rule keeps a
// tiny always-on-top overlay (a 120x16 password-manager bar, a clock widget)
// from silencing the wallpaper forever.
function occlusionMonitorScript() {
  return [
    'Add-Type -TypeDefinition @"',
    'using System;',
    'using System.Collections.Generic;',
    'using System.Runtime.InteropServices;',
    'using System.Text;',
    '',
    'public class Occ {',
    '  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);',
    '  public delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data);',
    '',
    '  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);',
    '  [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);',
    '  [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO mi);',
    '  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);',
    '  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);',
    '  [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();',
    '  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);',
    '  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);',
    '  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);',
    '',
    '  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }',
    '  [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }',
    '',
    '  static long Area(RECT r) {',
    '    long w = (long)r.Right - r.Left;',
    '    long h = (long)r.Bottom - r.Top;',
    '    return (w > 0 && h > 0) ? w * h : 0;',
    '  }',
    '',
    '  static List<RECT> Monitors() {',
    '    var list = new List<RECT>();',
    '    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr h, IntPtr hdc, ref RECT r, IntPtr d) {',
    '      var mi = new MONITORINFO();',
    '      mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));',
    '      if (GetMonitorInfo(h, ref mi)) list.Add(mi.rcMonitor);',
    '      return true;',
    '    }, IntPtr.Zero);',
    '    return list;',
    '  }',
    '',
    '  static bool Cloaked(IntPtr h) {',
    '    int cloaked = 0;',
    '    int hr = DwmGetWindowAttribute(h, 14, out cloaked, sizeof(int));',
    '    return hr == 0 && cloaked != 0;',
    '  }',
    '',
    '  static string ClassName(IntPtr h) {',
    '    var sb = new StringBuilder(256);',
    '    GetClassName(h, sb, sb.Capacity);',
    '    return sb.ToString();',
    '  }',
    '',
    '  // EnumWindows walks top-to-bottom in Z-order, so the first window that',
    '  // meaningfully covers a monitor decides. The desktop sits at the bottom of',
    '  // the normal stack, so reaching it means nothing above it is covering the',
    '  // screen - and Win+D raises it to the top, which is exactly the "desktop',
    '  // visible" case. Skipping it instead (the first version) kept the desktop',
    '  // muted forever after Win+D.',
    '  public static string Scan() {',
    '    IntPtr shell = GetShellWindow();',
    '    List<RECT> monitors = Monitors();',
    '    string found = null;',
    '    EnumWindows(delegate(IntPtr h, IntPtr l) {',
    '      if (found != null) return false;',
    '      if (!IsWindowVisible(h) || IsIconic(h)) return true;',
    '      string cls = ClassName(h);',
    '      // The taskbar is always above normal windows; it is not content.',
    '      if (cls == "Shell_TrayWnd" || cls == "Shell_SecondaryTrayWnd") return true;',
    '      if (Cloaked(h)) return true;',
    '      RECT r;',
    '      if (!GetWindowRect(h, out r)) return true;',
    '      if (Area(r) <= 0) return true;',
    '      long bestW = 0;',
    '      long bestH = 0;',
    '      foreach (RECT m in monitors) {',
    '        long iw = Math.Min((long)r.Right, m.Right) - Math.Max((long)r.Left, m.Left);',
    '        long ih = Math.Min((long)r.Bottom, m.Bottom) - Math.Max((long)r.Top, m.Top);',
    '        if (iw <= 0 || ih <= 0) continue;',
    '        long inter = iw * ih;',
    '        long threshold = (long)(Area(m) * 0.005);',
    '        if (threshold < 4096) threshold = 4096;',
    '        if (inter >= threshold && inter > bestW * bestH) {',
    '          bestW = iw;',
    '          bestH = ih;',
    '        }',
    '      }',
    '      if (bestW == 0) return true;',
    '      if (h == shell || cls == "Progman" || cls == "WorkerW" || cls == "SHELLDLL_DefView") {',
    '        return false;',
    '      }',
    '      found = cls + " " + bestW + "x" + bestH;',
    '      return false;',
    '    }, IntPtr.Zero);',
    '    return found ?? "";',
    '  }',
    '}',
    '"@',
    '$ErrorActionPreference = "Stop"',
    '$last = "<init>"',
    'while ($true) {',
    '  $state = ""',
    '  try { $state = [Occ]::Scan() } catch { $state = "" }',
    '  if ($state -ne $last) {',
    '    if ($state -eq "") { [Console]::Out.WriteLine("CLEAR") }',
    '    else { [Console]::Out.WriteLine("OCCLUDED " + $state) }',
    '    [Console]::Out.Flush()',
    '    $last = $state',
    '  }',
    '  Start-Sleep -Milliseconds 500',
    '}',
  ].join('\r\n')
}

function occlusionScriptPath() {
  return path.join(app.getPath('temp'), 'paiint-occlusion.ps1')
}

function scheduleOcclusionRestart() {
  if (isQuitting || occlusionRestartTimer) return
  if (!audioOnlyWhenClosed()) return
  if (lastApplied?.kind !== 'video') return
  const delay = occlusionRestartDelay
  occlusionRestartDelay = Math.min(occlusionRestartDelay * 2, 30_000)
  occlusionRestartTimer = setTimeout(() => {
    occlusionRestartTimer = null
    startOcclusionMonitor()
  }, delay)
  occlusionRestartTimer.unref?.()
  logLine('audio: occlusion monitor restart in', delay, 'ms')
}

function startOcclusionMonitor() {
  if (occlusionMonitor) return
  if (!audioOnlyWhenClosed()) return
  // Still images and an idle tray have no audio to mute.
  if (lastApplied?.kind !== 'video') return
  if (occlusionRestartTimer) {
    clearTimeout(occlusionRestartTimer)
    occlusionRestartTimer = null
  }
  try {
    const scriptPath = occlusionScriptPath()
    fs.writeFileSync(scriptPath, occlusionMonitorScript(), 'utf8')
    const child = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', scriptPath], {
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    })
    occlusionMonitor = child
    logLine('audio: occlusion monitor started')
    child.stdout.setEncoding('utf8')
    let buf = ''
    child.stdout.on('data', (chunk) => {
      buf += chunk
      let idx
      while ((idx = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, idx).trim()
        buf = buf.slice(idx + 1)
        const state = parseOcclusionLine(line)
        if (!state) continue
        // The first report switches the decision from the Paiint-window
        // fallback to the enumeration, so it must be applied even when it
        // matches the initial `false`.
        const firstReport = !occlusionReported
        occlusionReported = true
        occlusionRestartDelay = 1000
        if (!firstReport && state.occluded === desktopOccluded) continue
        desktopOccluded = state.occluded
        occlusionDetail = state.detail
        logLine('audio: desktop', state.occluded ? `occluded by ${state.detail || 'window'}` : 'clear')
        diagnostics?.event('occlusion', { occluded: state.occluded, detail: state.detail })
        applyEffectiveMute('occlusion').catch(() => {})
      }
    })
    let stderr = ''
    child.stderr.setEncoding('utf8')
    child.stderr.on('data', (chunk) => {
      stderr = (stderr + chunk).slice(-2000)
    })
    child.on('error', (err) => {
      logLine('audio: occlusion monitor error', err.message)
      if (occlusionMonitor === child) occlusionMonitor = null
      scheduleOcclusionRestart()
    })
    child.on('exit', (code) => {
      if (occlusionMonitor === child) occlusionMonitor = null
      if (stderr.trim()) logLine('audio: occlusion monitor stderr', stderr.trim().slice(0, 500))
      if (!isQuitting && audioOnlyWhenClosed()) logLine('audio: occlusion monitor exited', code)
      scheduleOcclusionRestart()
    })
    child.unref?.()
  } catch (err) {
    logLine('audio: occlusion monitor failed to start', err.message)
    scheduleOcclusionRestart()
  }
}

function stopOcclusionMonitor() {
  if (occlusionRestartTimer) {
    clearTimeout(occlusionRestartTimer)
    occlusionRestartTimer = null
  }
  occlusionRestartDelay = 1000
  const child = occlusionMonitor
  occlusionMonitor = null
  if (child) {
    try { child.kill() } catch {}
  }
  desktopOccluded = false
  occlusionDetail = ''
  occlusionReported = false
  try { fs.rmSync(occlusionScriptPath(), { force: true }) } catch {}
}
function diagnosticsPreferredDir() {
  return path.join(app.getPath('downloads'), 'Paiint Diagnostics')
}

function diagnosticsRoot() {
  const preferred = diagnosticsPreferredDir()
  try {
    fs.mkdirSync(preferred, { recursive: true })
    // mkdir succeeds on an existing folder even when it rejects writes, so
    // prove a write before the recorder commits its session there.
    const probe = path.join(preferred, '.write-probe')
    fs.writeFileSync(probe, '')
    fs.rmSync(probe, { force: true })
    return { dir: preferred, fallback: false }
  } catch (err) {
    const fallback = path.join(app.getPath('userData'), 'diagnostics')
    fs.mkdirSync(fallback, { recursive: true })
    logLine('diagnostics: Downloads not writable, using', fallback, err.message)
    return { dir: fallback, fallback: true }
  }
}

// One failing Electron API must not erase the rest of the sample. Real tester
// sessions contained only {"error":"powerMonitor.getIdleTime is not a
// function"} for every sample, so window, power, memory and audio state were
// invisible. Each section fails on its own now.
function sampleField(fn) {
  try {
    return fn()
  } catch (err) {
    return { error: err?.message || String(err) }
  }
}

function collectDiagnosticsSample() {
  const win = mainWindow
  return {
    app: sampleField(() => ({
      uptimeSec: Math.round(process.uptime()),
      memory: process.memoryUsage(),
      freeMemory: os.freemem(),
      totalMemory: os.totalmem(),
      metrics: app.getAppMetrics().map((m) => ({
        pid: m.pid,
        type: m.type,
        cpu: m.cpu,
        workingSetSize: m.memory?.workingSetSize ?? null,
        privateBytes: m.memory?.privateBytes ?? null,
      })),
    })),
    window: sampleField(() => {
      if (!win || win.isDestroyed()) return { exists: false, open: false }
      return {
        exists: true,
        open: isWindowOpen(win),
        visible: win.isVisible(),
        minimized: win.isMinimized(),
        focused: win.isFocused(),
        bounds: win.getBounds(),
      }
    }),
    displays: sampleField(() =>
      screen.getAllDisplays().map((d) => ({
        id: d.id,
        bounds: d.bounds,
        workArea: d.workArea,
        scaleFactor: d.scaleFactor,
        rotation: d.rotation,
        internal: d.internal,
        displayFrequency: d.displayFrequency,
        colorSpace: d.colorSpace,
        colorDepth: d.colorDepth,
      })),
    ),
    power: sampleField(() => ({
      onBattery: powerMonitor.isOnBatteryPower(),
      // Electron exposes getSystemIdleTime() (seconds); getIdleTime() does
      // not exist and used to throw away the whole sample.
      idleTime:
        typeof powerMonitor.getSystemIdleTime === 'function'
          ? powerMonitor.getSystemIdleTime()
          : null,
      thermalState:
        typeof powerMonitor.getCurrentThermalState === 'function'
          ? powerMonitor.getCurrentThermalState()
          : null,
    })),
    lastApplied: sampleField(() =>
      lastApplied ? { kind: lastApplied.kind, ...lastApplied.params } : null,
    ),
    playlist: sampleField(() => {
      const playlist = playlistScheduler?.state?.()
      return playlist
        ? {
            active: playlist.active,
            playlistId: playlist.playlistId,
            playlistName: playlist.playlistName,
            mode: playlist.mode,
            currentPath: playlist.currentPath,
            queueLength: playlist.queueLength,
            nextDueAt: playlist.nextDueAt,
            exhausted: playlist.exhausted,
          }
        : null
    }),
    audio: sampleField(() => {
      const decision = audioDecision()
      return {
        onlyWhenClosed: decision.onlyWhenClosed,
        intentMuted: decision.intentMuted,
        windowOpen: decision.windowOpen,
        desktopOccluded: decision.desktopOccluded,
        occlusionReported: decision.occlusionReported,
        occlusionDetail,
        effectiveMuted: lastApplied?.kind === 'video' ? decision.muted : null,
      }
    }),
  }
}

function diagnosticsStatus() {
  const s = diagnostics?.status()
  return {
    enabled: !!s?.enabled,
    forced: diagnosticsForced,
    dir: s?.dir ?? diagnosticsRootInfo?.dir ?? diagnosticsPreferredDir(),
    sessionDir: s?.sessionDir ?? null,
    bytes: s?.bytes ?? 0,
    samples: s?.samples ?? 0,
    events: s?.events ?? 0,
    lastSampleAt: s?.lastSampleAt ?? null,
    capReached: s?.capReached ?? false,
    fallback: s?.fallback ?? diagnosticsRootInfo?.fallback ?? false,
  }
}

async function startDiagnostics(reason) {
  if (diagnostics) return diagnosticsStatus()
  if (diagnosticsStart) return diagnosticsStart
  diagnosticsStart = (async () => {
    const root = diagnosticsRoot()
    diagnosticsRootInfo = root
    const tempDir = app.getPath('temp')
    const makeRecorder = (target) => new DiagnosticsRecorder({
      rootDir: target.dir,
      fallback: target.fallback,
      appVersion: app.getVersion(),
      forced: diagnosticsForced,
      log: logLine,
      getStatus: async () => {
        if (!runtime.proc) return { available: false, reason: 'runtime-stopped' }
        try {
          return { available: true, data: await runtime.send('GET_STATUS', {}, 1500) }
        } catch (err) {
          return {
            available: false,
            reason: /timeout/i.test(err.message) ? 'timeout' : 'error',
          }
        }
      },
      collectSample: collectDiagnosticsSample,
      logFiles: [
        { name: 'runtime.log', path: path.join(tempDir, 'Paiint', 'runtime.log') },
        { name: 'app.log', path: path.join(tempDir, 'paiint-app.log') },
        { name: 'crash.log', path: path.join(tempDir, 'Paiint', 'crash.log') },
      ],
      crashDir: path.join(tempDir, 'Paiint', 'crashes'),
    })

    let recorder = makeRecorder(root)
    let started = recorder.start()
    if (!started?.enabled && !root.fallback) {
      // The recorder catches its own start failure. A folder can exist yet
      // reject writes, so retry once under userData before giving up.
      logLine('diagnostics: preferred root failed, retrying fallback', root.dir)
      const fallback = { dir: path.join(app.getPath('userData'), 'diagnostics'), fallback: true }
      try {
        fs.mkdirSync(fallback.dir, { recursive: true })
        diagnosticsRootInfo = fallback
        recorder = makeRecorder(fallback)
        started = recorder.start()
      } catch (err) {
        logLine('diagnostics: fallback root failed', err.message)
        started = null
      }
    }
    if (!started?.enabled) {
      // Leave diagnostics null so a later toggle can retry instead of
      // short-circuiting on a dead recorder.
      logLine('diagnostics: recorder could not start')
      return diagnosticsStatus()
    }
    diagnostics = recorder
    logLine('diagnostics: recorder started', diagnosticsRootInfo.dir, reason)
    try {
      recorder.setDeviceInfo(await collectDeviceInfo({ app, screen }))
    } catch (err) {
      logLine('diagnostics: device info failed', err.message)
    }
    return diagnosticsStatus()
  })().finally(() => {
    diagnosticsStart = null
  })
  return diagnosticsStart
}

function waitForDiagnosticsStart(ms) {
  if (!diagnosticsStart) return Promise.resolve()
  return Promise.race([
    diagnosticsStart.catch(() => {}),
    new Promise((resolve) => {
      const timer = setTimeout(resolve, ms)
      timer.unref?.()
    }),
  ])
}

async function stopDiagnostics(reason) {
  // Let a just-started recorder finish writing device.json before the session
  // is archived. Bounded: a slow WMI/GPU probe must not block the stop.
  await waitForDiagnosticsStart(2500)
  if (!diagnostics) return diagnosticsStatus()
  try {
    diagnostics.archive(reason)
  } catch (err) {
    logLine('diagnostics: archive failed', err.message)
  }
  diagnostics = null
  logLine('diagnostics: recorder stopped', reason)
  return diagnosticsStatus()
}

// Where the native runtime writes the quit/stop still. Kept in sync with
// LastFrameImagePath() in rex-core/src/wallpaper_cli_main.cpp.
function lastFrameSnapshotPath() {
  // The real Pictures folder, not AppData: MSIX redirects AppData into the
  // package's LocalCache, where Explorer cannot use it as a wallpaper.
  try {
    const pictures = app.getPath('pictures')
    return pictures ? path.join(pictures, 'Paiint', 'last-frame.jpg') : null
  } catch {
    return null
  }
}

// Playlist dock "stop": end the rotation and leave the last rendered frame on
// the desktop as a still. The native SHUTDOWN path captures the frame, hands
// it to the OS wallpaper API and only then tears WorkerW down, so the desktop
// never flashes black. `intentional` keeps the supervisor out of recovery.
async function stopWallpaperForSnapshot() {
  if (playlistScheduler) playlistScheduler.setPlan(null)
  // Stop is session-scoped: the desktop now holds a still, not a scene the
  // runtime can replay. Clearing the in-memory record keeps a recreated
  // window (closed to tray, then reopened) from re-adopting the video and
  // resuming the rotation; last-wallpaper.json is untouched, so a full app
  // restart still restores the wallpaper.
  lastApplied = null
  stopOcclusionMonitor()
  if (!runtime.proc) {
    logLine('wallpaper:stop: no runtime running')
    return { ok: true, stopped: false, snapshot: lastFrameSnapshotPath() }
  }
  capabilities = null
  // Wait for the capture: the caller gets a snapshot path that already
  // exists, and Stop has finished before the dock reports success.
  await runtime.stop({ snapshot: true, intentional: true })
  logLine('wallpaper:stop: rotation stopped, last frame left on the desktop')
  return { ok: true, stopped: true, snapshot: lastFrameSnapshotPath() }
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
// Same LOAD/PLAY sequence as a user apply, hydrated from the library and
// verified against the desktop layer before it counts as restored.
async function applyStoredWallpaper(entry) {
  if (!entry || (entry.kind !== 'image' && entry.kind !== 'video')) return false
  try {
    const ok = await replayStoredEntry(entry, 'startup', { resetRecovery: true })
    if (!ok) {
      logLine('startup: re-apply desktop layer not verified')
      return false
    }
    logLine('startup: re-applied last wallpaper', entry.kind, entry.params.path)
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
async function applyImageEntry({ path: filePath, mode }, source = 'wallpaper:set', opts = {}) {
  if (typeof filePath !== 'string' || !IMAGE_EXT.test(filePath)) {
    throw new Error('unsupported file type')
  }
  const engineMode = mode ?? 'fill'
  diagnostics?.event('apply', { source, kind: 'image', path: filePath, mode: engineMode })
  // No INIT: LOAD applies the image through the OS wallpaper API and is valid
  // with no engine (the runtime spawns on demand). INIT would build a 4K D3D12
  // engine the image never uses - measured 84 MB private and ~103 MB VRAM.
  await runtime.send('LOAD', { path: filePath, type: 'image', mode: engineMode })
  capabilities = null // adapter changed - refresh on next query
  lastApplied = { kind: 'image', params: { path: filePath, mode: engineMode } }
  // No video, no audio: stop the occlusion poller instead of leaving a
  // PowerShell process enumerating windows for nothing.
  stopOcclusionMonitor()
  startup.saveLastApplied(app.getPath('userData'), lastApplied)
  if (opts.resetRecovery !== false && supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
  setRuntimeAvailable(true)
  // The image is on the desktop now: free the engine instead of leaving it
  // warm for a video that may never come.
  reclaimIdleRuntime(`${source} image applied`)
  logLine(`${source}: image applied`, filePath)
  diagnostics?.event('applied', { source, kind: 'image', path: filePath })
  return { ok: true, style: engineMode }
}

// Apply one video scene. Same contract as applyImageEntry; mirrors the
// wallpaper:video handler (audio layout and colour are live SET_* commands,
// not part of LOAD).
async function applyVideoEntry(params, source = 'wallpaper:video', opts = {}) {
  const { path: filePath, mode, fps, loop, muted, volume, speed, audioMode, brightness, contrast, saturation } = params
  if (typeof filePath !== 'string' || !filePath) throw new Error('invalid video path')
  const intentMuted = muted ?? true
  diagnostics?.event('apply', { source, kind: 'video', path: filePath, mode, muted: muted ?? true })
  diagnostics?.markApply(source)
  const init = engineInitOptions()
  await runtime.init(init.maxFps, init.hardwareAcceleration)
  // Read the window state as late as possible: INIT can take ~0.7 s, and on a
  // boot restore the window may appear while it runs. A value captured before
  // INIT would send "audible" even though the window is already on screen.
  const effectiveMuted = effectiveVideoMute(intentMuted)
  await runtime.send('LOAD', {
    path: filePath,
    type: 'video',
    mode: mode ?? 'fill',
    loop: loop ?? true,
    muted: effectiveMuted,
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
    params: { path: filePath, mode, loop, muted: intentMuted, volume, speed, fps, audioMode, brightness, contrast, saturation },
  }
  // Reconcile once more now that lastApplied exists: a show/hide that landed
  // between the LOAD send and this assignment saw no video and was ignored.
  await applyEffectiveMute('video-applied')
  if (audioOnlyWhenClosed()) startOcclusionMonitor()
  startup.saveLastApplied(app.getPath('userData'), lastApplied)
  if (opts.resetRecovery !== false && supervisor) supervisor.resetRecovery() // deliberate apply clears parked recovery
  setRuntimeAvailable(true)
  logLine(`${source}: video applied`, filePath)
  diagnostics?.event('applied', { source, kind: 'video', path: filePath })
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
    icon: path.join(__dirname, '..', 'assets', 'paiint-app.ico'),
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

  // Wallpaper audio follows whether a Paiint window is actually on screen.
  // hide (close-to-tray), minimize and closed all mean "not open".
  for (const eventName of ['show', 'hide', 'minimize', 'restore']) {
    win.on(eventName, () => {
      diagnostics?.event('window', { event: eventName, open: isWindowOpen(win) })
      applyEffectiveMute(`window-${eventName}`).catch(() => {})
    })
  }

  // Close → tear the renderer down. Paiint lives in the tray, and the native
  // runtime is a separate process that keeps the wallpaper playing. Destroying
  // (rather than hiding) is what actually releases the renderer and its GPU
  // allocations; a hidden window otherwise keeps both for the app's lifetime.
  //
  // A running playlist is NOT a reason to keep the renderer alive any more:
  // its Timer/scene-end scheduler is in main (playlist-scheduler.cjs), so the
  // rotation continues in the tray.
  //
  // Closing quits by default (tray.closeToTray === false), so the runtime
  // stops and leaves the last frame as a still. Turning the Settings option
  // off keeps the app alive in the tray and the wallpaper playing.
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
    diagnostics?.event('window', { event: 'closed' })
    applyEffectiveMute('window-closed').catch(() => {})
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
    // FRAME_READY is a ~30 Hz presentation heartbeat. Logging it filled a
    // 1.2 MB session in three minutes and reached the 200 MB cap in hours;
    // the engine telemetry in each sample already carries fps/present counts.
    if (event !== 'FRAME_READY') diagnostics?.event('runtime-event', { event, data })
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
    diagnostics?.event('wallpaper-update', { mode, loop, volume, speed, muted, audioMode, brightness, contrast, saturation })
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
    if (muted != null) {
      // Send the visibility-aware value; mergeLiveUpdate below still persists the
      // user's intent (`muted`), never the temporary override.
      await sendIfSupported('SET_MUTE', { value: effectiveVideoMute(!!muted) }, applied, skipped)
    }
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
  // Playlist dock "stop": the renderer clears the rotation plan, then asks for
  // the runtime to capture its last frame and apply it as a still before the
  // process exits. The runtime is tagged intentional so the supervisor does
  // not treat the exit as a crash and replay the video.
  ipcMain.handle('wallpaper:stop', () => stopWallpaperForSnapshot())

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
      const next = appState.set(keyPath, value) ?? null
      if (keyPath === 'engine.audioOnlyWhenWindowClosed') {
        diagnostics?.event('setting', { keyPath, value })
        if (value === true) startOcclusionMonitor()
        else stopOcclusionMonitor()
        applyEffectiveMute('setting').catch(() => {})
      }
      if (keyPath === 'diagnostics.enabled') {
        diagnostics?.event('setting', { keyPath, value })
        if (value === true) {
          startDiagnostics('setting').catch((err) => logLine('diagnostics: start ERROR', err.message))
        } else {
          stopDiagnostics('setting').catch((err) => logLine('diagnostics: stop ERROR', err.message))
        }
      }
      // The portable Run key bakes --background into its command line, so a
      // changed "start minimised" only reaches the next logon after the entry
      // is rewritten. The MSIX startup task reads the preference at launch
      // (and cannot take arguments), so only the Run-key path needs this.
      if (keyPath === 'startup.startMinimized' && startupTaskState() === null) {
        try {
          if (startup.loginItemEnabled(app.getLoginItemSettings())) applyLoginItem(true)
        } catch (err) {
          logLine('app-state:set login-item refresh ERROR', err.message)
        }
      }
      return next
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

  // Local-only test-mode recorder. status() is polled whenever Settings opens,
  // so it must never create the folder (see diagnosticsPreferredDir).
  ipcMain.handle('diagnostics:status', () => diagnosticsStatus())
  ipcMain.handle('diagnostics:open-folder', async () => {
    let dir = diagnosticsRootInfo?.dir ?? diagnosticsPreferredDir()
    try {
      const root = diagnosticsRootInfo ?? diagnosticsRoot()
      diagnosticsRootInfo = root
      dir = root.dir
      fs.mkdirSync(dir, { recursive: true })
      const openError = await shell.openPath(dir)
      if (openError) return { ok: false, dir, error: openError }
      return { ok: true, dir }
    } catch (err) {
      logLine('diagnostics: open-folder ERROR', err.message)
      return { ok: false, dir, error: err.message }
    }
  })

  // Legal documents ship as resources/legal/*.md (extraResources) so the
  // About page can show the exact text that came with the build; in dev they
  // are read from the repo's docs/legal. Allowlisted by key — the renderer
  // can never ask for an arbitrary file.
  ipcMain.handle('app:read-legal', async (_event, { target } = {}) => {
    const doc = legalDocPath(target, {
      packaged: app.isPackaged,
      resourcesPath: process.resourcesPath,
      electronDir: __dirname,
    })
    if (!doc) return { ok: false, reason: 'unknown-target' }
    try {
      const text = await fs.promises.readFile(doc.path, 'utf8')
      return { ok: true, title: doc.title, text }
    } catch (err) {
      logLine('app:read-legal ERROR', target, err.message)
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
  // Local-only test mode: --diagnostics forces one recorded session without
  // touching the persisted setting.
  diagnosticsForced = process.argv.includes('--diagnostics')
  if (diagnosticsForced || appState.get('diagnostics.enabled') === true) {
    setImmediate(() => {
      // The setting may have been turned off before this tick.
      if (!diagnosticsForced && appState.get('diagnostics.enabled') !== true) return
      startDiagnostics(diagnosticsForced ? 'flag' : 'setting')
        .catch((err) => logLine('diagnostics: start ERROR', err.message))
    })
  }
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
      appState.set('settings.loginItemInitialized', true)
      // The task helper is a child process (query + request); keep it off the
      // startup path so the window and the wallpaper are not delayed by it.
      setImmediate(() => {
        try {
          applyLoginItem(true)
          logLine('startup: registered for login (first run)')
        } catch (err) {
          logLine('startup: first-run login-item ERROR', err.message)
        }
      })
    } else {
      logLine('startup: login item left as configured (enabled=' + loginItemEnabled() + ')')
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
      diagnostics?.event('supervisor-event', msg)
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

  // Normal launch opens the UI; a login launch (--background from the Run key,
  // --startup-task from the MSIX stub) starts the tray + native services only
  // and defers the window to the first tray open. The startup task launches
  // without --background, so it honours the live "start minimised" preference.
  const startInBackground =
    process.argv.includes('--background') ||
    (startupTaskLaunch && appState.get('startup.startMinimized') !== false)
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
    logLine(
      startupTaskLaunch
        ? 'window: deferred (startup task, start minimised)'
        : 'window: deferred (background launch)',
    )
  }

  app.on('activate', () => {
    showMainWindow()
  })
})

// Paiint is a background application — the tray owns the lifetime. Closing the
// window destroys it (see the close handler) but leaves the app and the native
// runtime running; only Exit Paiint (quitApp) terminates the process.
app.on('window-all-closed', () => {})

// The quit is drained exactly once. `will-quit` is re-entered by our own
// app.quit() after the runtime has exited; `quitDrainFinished` is what lets
// that second pass through.
let quitDrainStarted = false
let quitDrainFinished = false
app.on('will-quit', (event) => {
  // Second pass: the drain is done, let Electron finish the quit.
  if (quitDrainFinished) return
  // A drain is already in flight (a logoff can fire will-quit again): keep
  // waiting rather than killing the runtime mid-capture.
  event.preventDefault()
  if (quitDrainStarted) return
  quitDrainStarted = true

  try {
    if (supervisor) supervisor.stop() // quitting: exits must not trigger recovery
    if (playlistScheduler) playlistScheduler.stop() // stop timers; plan is already persisted
    stopOcclusionMonitor()
    // The scheduler's last change may still sit in the app-state debounce.
    try { appState?.flush() } catch {}
  } catch (err) {
    // Never leave the quit prevented with nothing scheduled: a throw here
    // used to strand the app with no window and no tray.
    logLine('app: quit cleanup ERROR', err.message)
  }

  // Exit leaves the last video frame on the desktop as a still (the runtime
  // captures it and applies it through the OS wallpaper API) unless the user
  // turned that off in Settings -> Engine. Idle reclaims pass no snapshot, so
  // switching to an image never gets overwritten.
  const snapshotOnQuit = appState?.get('engine.snapshotOnQuit') !== false
  // Wait for the native SHUTDOWN to finish before Electron exits. A snapshot
  // quit takes seconds (capture + PNG encode + shell apply); quitting
  // immediately killed the runtime mid-capture, so the desktop never got the
  // still and the setting looked broken. The runtime wrapper bounds the wait
  // (15 s snapshot / 5 s plain) and resolves on exit or deadline.
  const finish = () => {
    if (quitDrainFinished) return
    logLine('app: runtime drained, quitting')
    // A start still collecting device info would otherwise be archived without
    // device.json. Bounded so a slow WMI/GPU probe cannot stall the quit.
    const archive = () => {
      if (quitDrainFinished) return
      quitDrainFinished = true
      // Runs after runtime.stop() resolved, so runtime.log is complete before
      // the session folder is archived.
      try {
        diagnostics?.archive('quit')
      } catch (err) {
        logLine('diagnostics: quit archive failed', err.message)
      }
      // MUST be a macrotask. When no runtime is running (every still-image
      // user) runtime.stop() resolves immediately, and a promise callback runs
      // as a microtask INSIDE this will-quit emission - Electron's quit flag is
      // still set there, so app.quit() would be dropped and the process would
      // live on with no window and no tray.
      setImmediate(() => app.quit())
    }
    waitForDiagnosticsStart(2500).then(archive, archive)
  }
  let drain = null
  try {
    drain = runtime.stop({ snapshot: snapshotOnQuit })
  } catch (err) {
    logLine('app: quit drain ERROR', err.message)
  }
  Promise.resolve(drain).then(finish, finish)
})
