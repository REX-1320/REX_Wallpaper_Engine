const { app, BrowserWindow, shell, ipcMain } = require('electron')
const path = require('node:path')
const fs = require('node:fs')
const { WallpaperRuntime } = require('./wallpaperRuntime.cjs')

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
  return process.resourcesPath
    ? path.join(process.resourcesPath, 'bin', 'rex-wallpaper-cli.exe')
    : path.join(__dirname, '..', '..', 'rex-core', '.build', 'Release', 'rex-wallpaper-cli.exe')
}

// ─── Persistent Wallpaper Runtime ──────────────────────────────────────────
// Spawned once, kept alive, driven by LOAD/PLAY/SET_* commands. The engine
// stays warm (D3D12 + MediaFoundation), so applies are fast scene swaps
// instead of process restarts.

const runtime = new WallpaperRuntime(getWallpaperCliPath(), logLine)

let capabilities = null

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
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  })

  win.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })
}

app.whenReady().then(() => {
  runtime.on('event', (event, data) => {
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
      await runtime.send('LOAD', { path: filePath, type: 'image', mode })
      capabilities = null // adapter changed — refresh on next query
      return { ok: true, style: mode }
    } catch (err) {
      logLine('wallpaper:set ERROR', err.message)
      return { ok: false, error: err.message }
    }
  })

  ipcMain.handle('wallpaper:video', async (event, { path: filePath, mode, fps, loop, muted, volume, speed } = {}) => {
    logLine('wallpaper:video', JSON.stringify({ path: filePath, mode, fps, loop, muted, volume, speed }))

    if (typeof filePath !== 'string' || !filePath) {
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

  ipcMain.handle('shell:show-in-folder', (event, { path: filePath } = {}) => {
    if (typeof filePath === 'string' && filePath) shell.showItemInFolder(filePath)
    return { ok: true }
  })

  createWindow()
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})

app.on('will-quit', () => {
  runtime.stop()
})
