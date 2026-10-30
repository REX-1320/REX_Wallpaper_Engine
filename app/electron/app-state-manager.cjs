const fs = require('node:fs')
const path = require('node:path')
const { app } = require('electron')

const CURRENT_STATE_VERSION = 1
const BACKUP_KEEP = 14

class AppStateManager {
  constructor() {
    this.file = path.join(app.getPath('userData'), 'app-state.json')
    this.backupDir = path.join(app.getPath('userData'), 'config_backups')
    this.state = this.defaultState()
    this.timer = null
    this.loadWithRecovery()
  }

  defaultState() {
    return {
      version: CURRENT_STATE_VERSION,
      window: { x: null, y: null, width: 1280, height: 720, maximized: false },
      ui: {
        lastPage: 'Wallpapers',
        sidebarCollapsed: false,
        inspectorVisible: true,
        // Remember the window's size and position between launches.
        rememberWindowBounds: true,
        // Gallery <-> inspector split (px). null = even 50/50 split.
        gallerySplitPx: null,
        // Bottom playlist dock.
        panelHeight: 220,
        panelOpen: true,
        panelMinimized: false,
        panelMaximized: false,
        panelPlaylistId: null,
      },
      recent: { wallpapers: [], wallpaperIndex: -1 },
      theme: { themeId: 'midnight-glass', colorTokens: {} },
      // Closing the window quits Paiint by default (the desktop keeps the
      // last frame as a still). Opt in to the tray with Settings > Window.
      tray: { minimizeToTray: true, closeToTray: false },
      engine: {
        maxFps: 0,
        hardwareAcceleration: true,
        snapshotOnQuit: true,
        // Mute the wallpaper while a Paiint window is open. The per-wallpaper
        // playback.mute remains the user's intent; this is a visibility override.
        audioOnlyWhenWindowClosed: false,
      },
      // Local-only test-mode recorder. Off by default; --diagnostics forces it on
      // for one session without changing this value.
      diagnostics: { enabled: false },
      wallpaperSettings: {}, // per-wallpaper settings keyed by normalized path (Phase 2)
      wallpaper: { assignments: {}, playback: { position: 0, volume: 1.0, paused: false } },
      playlistEngine: { playlists: [], rotation: null }, // UI playlists + the main-process rotation plan
    }
  }

  // ─── Load with corruption recovery ──────────────────────────────────────
  loadWithRecovery() {
    try {
      if (!fs.existsSync(this.file)) {
        this.state = this.defaultState()
        this.createDailyBackup()
        return
      }
      const raw = fs.readFileSync(this.file, 'utf8')
      const parsed = JSON.parse(raw)
      this.state = this.mergeWithDefaults(parsed)
      this.createDailyBackup()
    } catch (err) {
      console.error('[AppState] Corrupted state file:', err.message)
      this.quarantineCorruptFile()
      const restored = this.restoreFromNewestBackup()
      if (restored) {
        console.log('[AppState] Restored from backup')
        this.state = this.mergeWithDefaults(restored)
      } else {
        console.warn('[AppState] No backup found, using defaults')
        this.state = this.defaultState()
      }
      // Persist a fresh valid file after recovery.
      this.flush()
    }
  }

  quarantineCorruptFile() {
    const corruptPath = this.file + '.corrupt'
    try {
      fs.renameSync(this.file, corruptPath)
    } catch (err) {
      console.error('[AppState] Failed to quarantine corrupt file:', err.message)
    }
  }

  restoreFromNewestBackup() {
    if (!fs.existsSync(this.backupDir)) return null
    const files = fs.readdirSync(this.backupDir)
      .filter((f) => f.startsWith('config_') && f.endsWith('.json'))
      .sort()
      .reverse()
    if (files.length === 0) return null
    const newest = path.join(this.backupDir, files[0])
    try {
      const raw = fs.readFileSync(newest, 'utf8')
      return JSON.parse(raw)
    } catch (err) {
      console.error('[AppState] Backup file corrupted:', err.message)
      return null
    }
  }

  createDailyBackup() {
    try {
      fs.mkdirSync(this.backupDir, { recursive: true })
      const today = new Date().toISOString().slice(0, 10)
      const backupFile = path.join(this.backupDir, `config_${today}.json`)
      if (!fs.existsSync(backupFile)) {
        fs.writeFileSync(backupFile, JSON.stringify(this.state, null, 2))
      }
      this.pruneBackups()
    } catch (err) {
      console.error('[AppState] Backup creation failed:', err.message)
    }
  }

  pruneBackups() {
    try {
      if (!fs.existsSync(this.backupDir)) return
      const files = fs.readdirSync(this.backupDir)
        .filter((f) => f.startsWith('config_') && f.endsWith('.json'))
        .sort()
        .reverse()
      for (let i = BACKUP_KEEP; i < files.length; i++) {
        fs.unlinkSync(path.join(this.backupDir, files[i]))
      }
    } catch (err) {
      console.error('[AppState] Backup pruning failed:', err.message)
    }
  }

  save() {
    // Debounce writes to avoid disk thrash during rapid updates.
    if (this.timer) clearTimeout(this.timer)
    this.timer = setTimeout(() => {
      this.timer = null
      this.flush()
    }, 300)
  }

  flush() {
    if (this.timer) {
      clearTimeout(this.timer)
      this.timer = null
    }
    const tmpFile = this.file + '.tmp'
    try {
      fs.mkdirSync(path.dirname(this.file), { recursive: true })
      // Atomic write: temp file + rename, so a crash mid-write can never
      // truncate the live state file.
      fs.writeFileSync(tmpFile, JSON.stringify(this.state, null, 2))
      fs.renameSync(tmpFile, this.file)
      // Keep today's backup in sync with the latest valid state.
      try {
        fs.mkdirSync(this.backupDir, { recursive: true })
        const today = new Date().toISOString().slice(0, 10)
        fs.copyFileSync(this.file, path.join(this.backupDir, `config_${today}.json`))
        this.pruneBackups()
      } catch (err) {
        console.error('[AppState] Backup update failed:', err.message)
      }
    } catch (err) {
      console.error('[AppState] flush failed:', err.message)
      try {
        if (fs.existsSync(tmpFile)) fs.unlinkSync(tmpFile)
      } catch {}
    }
  }

  mergeWithDefaults(saved) {
    const def = this.defaultState()
    return {
      ...def,
      ...saved,
      window: { ...def.window, ...(saved.window || {}) },
      ui: { ...def.ui, ...(saved.ui || {}) },
      recent: { ...def.recent, ...(saved.recent || {}) },
      theme: { ...def.theme, ...(saved.theme || {}) },
      tray: { ...def.tray, ...(saved.tray || {}) },
      engine: { ...def.engine, ...(saved.engine || {}) },
      diagnostics: { ...def.diagnostics, ...(saved.diagnostics || {}) },
      wallpaperSettings: { ...(saved.wallpaperSettings || {}) },
      wallpaper: {
        ...def.wallpaper,
        ...(saved.wallpaper || {}),
        playback: { ...def.wallpaper.playback, ...(saved.wallpaper?.playback || {}) },
      },
      // The defaults must be spread FIRST: a state file written before the
      // playlist key existed would otherwise drop `playlists` entirely.
      playlistEngine: { ...def.playlistEngine, ...(saved.playlistEngine || {}) },
    }
  }

  get(keyPath) {
    // Supports dot-separated path like "window.x"
    return keyPath.split('.').reduce((acc, part) => acc?.[part], this.state)
  }

  set(keyPath, value) {
    const keys = keyPath.split('.')
    let obj = this.state
    for (let i = 0; i < keys.length - 1; i++) {
      // Create missing intermediates so set() on a never-existing nested key
      // works instead of throwing (get() was already safe). A primitive in
      // the way is replaced — it cannot hold the new subtree.
      const k = keys[i]
      if (obj[k] === null || typeof obj[k] !== 'object') obj[k] = {}
      obj = obj[k]
    }
    obj[keys[keys.length - 1]] = value
    this.save()
    return value
  }

  update(partial) {
    this.state = this.mergeWithDefaults({ ...this.state, ...partial })
    this.save()
    return this.state
  }
}

module.exports = { AppStateManager, CURRENT_STATE_VERSION }
