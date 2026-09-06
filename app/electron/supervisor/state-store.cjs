'use strict'

// ─── StateStore ─────────────────────────────────────────────────────────────
// Persists supervisor recovery state (the last successfully applied wallpaper
// and its playback parameters) across app restarts so a crashed Electron main
// process can still restore the desktop wallpaper on the next recovery.
//
// File: <userData>/supervisor-state.json — small, atomic (tmp+rename) writes.

const fs = require('node:fs')
const path = require('node:path')

const DEFAULT_STATE = Object.freeze({
  lastWallpaper: null, // absolute path of the last successfully applied wallpaper
  wallpaperType: null, // 'image' | 'video'
  fillMode: 'fill',
  loop: true,
  muted: true,
  volume: 1,
  speed: 1,
  playbackPosition: 0,
  monitorId: null,
  generation: 0, // increments on every successful apply
  updatedAt: null,
})

class StateStore {
  constructor(filePath) {
    this.filePath = filePath
    this.state = this._load()
  }

  _load() {
    try {
      if (!fs.existsSync(this.filePath)) return { ...DEFAULT_STATE }
      const raw = fs.readFileSync(this.filePath, 'utf8')
      const parsed = JSON.parse(raw)
      if (typeof parsed !== 'object' || parsed === null) return { ...DEFAULT_STATE }
      return { ...DEFAULT_STATE, ...parsed }
    } catch {
      return { ...DEFAULT_STATE }
    }
  }

  _save() {
    try {
      fs.mkdirSync(path.dirname(this.filePath), { recursive: true })
      const tmp = `${this.filePath}.tmp`
      fs.writeFileSync(tmp, JSON.stringify(this.state, null, 2))
      fs.renameSync(tmp, this.filePath)
    } catch {
      // state persistence is best-effort; recovery still works in-memory
    }
  }

  // Records a successful wallpaper apply. Bumps the generation so downstream
  // consumers can tell the state advanced even when the file mtime is coarse.
  update(patch) {
    this.state = { ...this.state, ...patch, generation: this.state.generation + 1, updatedAt: new Date().toISOString() }
    this._save()
    return this.state
  }

  get() {
    return { ...this.state }
  }

  clear() {
    this.state = { ...DEFAULT_STATE }
    this._save()
  }
}

module.exports = { StateStore, DEFAULT_STATE }