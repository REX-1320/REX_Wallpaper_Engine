const fs = require('fs')
const path = require('path')
const { app, ipcMain } = require('electron')
const crypto = require('crypto')

class WallpaperLibrary {
  constructor(fileName = 'wallpaper-library.json') {
    this.filePath = path.join(app.getPath('userData'), fileName)
    this.items = []
    this.byPath = new Map()
    this.load()
    this.registerIpcHandlers()
  }

  // ── Persistence ───────────────────────────────────────────────────────
  load() {
    try {
      if (fs.existsSync(this.filePath)) {
        const raw = fs.readFileSync(this.filePath, 'utf8')
        const parsed = JSON.parse(raw)
        if (Array.isArray(parsed)) {
          this.items = parsed
        }
      }
    } catch (err) {
      console.error('[Library] Failed to load:', err.message)
      this.items = []
    }
    this.rebuildIndex()
  }

  flush() {
    const tmp = this.filePath + '.tmp'
    try {
      fs.writeFileSync(tmp, JSON.stringify(this.items, null, 2))
      fs.renameSync(tmp, this.filePath)
    } catch (err) {
      console.error('[Library] Flush failed:', err.message)
    }
  }

  rebuildIndex() {
    this.byPath.clear()
    for (let i = 0; i < this.items.length; i++) {
      const key = this.normalizePath(this.items[i].path)
      this.byPath.set(key, i)
    }
  }

  // ── Path normalization (case-insensitive, separator-normalized) ──────
  normalizePath(filePath) {
    return path.normalize(filePath).toLowerCase()
  }

  // ─── CRUD ─────────────────────────────────────────────────────────────
  async add(filePath, type = 'auto', extra = {}) {
    const normalized = this.normalizePath(filePath)
    if (!normalized) return null

    // Duplicate check FIRST
    if (this.byPath.has(normalized)) {
      const idx = this.byPath.get(normalized)
      let existing = { ...this.items[idx] }
      existing.lastUsed = new Date().toISOString()
      if (extra && typeof extra === 'object' && Object.keys(extra).length > 0) {
        existing = { ...existing, ...extra }
      }
      existing.path = filePath
      this.items[idx] = existing
      this.flush()
      return existing
    }

    // Determine type if auto
    let detectedType = type
    if (detectedType === 'auto') {
      const ext = path.extname(filePath).toLowerCase()
      if (['.jpg', '.jpeg', '.png', '.webp', '.bmp'].includes(ext)) detectedType = 'static'
      else if (['.mp4', '.webm', '.mov'].includes(ext)) detectedType = 'video'
      else if (['.hlsl', '.glsl'].includes(ext)) detectedType = 'shader'
      else detectedType = 'unknown'
    }

    // Basic metadata
    let metadata = {}
    try {
      const stat = fs.statSync(filePath)
      metadata = {
        size: stat.size,
        createdAt: (stat.birthtime || stat.ctime).toISOString(),
        modifiedAt: stat.mtime.toISOString(),
      }
    } catch {}

    const thumbnail = detectedType === 'static' ? filePath : ''

    const newItem = {
      id: crypto.randomUUID(),
      type: detectedType,
      path: filePath,
      title: path.basename(filePath, path.extname(filePath)),
      thumbnail,
      metadata,
      tags: extra.tags || [],
      favorite: false,
      addedAt: new Date().toISOString(),
      lastUsed: new Date().toISOString(),
    }

    this.items.push(newItem)
    this.byPath.set(normalized, this.items.length - 1)
    this.flush()
    return newItem
  }

  remove(filePath) {
    const normalized = this.normalizePath(filePath)
    if (!this.byPath.has(normalized)) return false

    const idx = this.byPath.get(normalized)
    this.items.splice(idx, 1)
    this.rebuildIndex()
    this.flush()
    return true
  }

  update(filePath, updates) {
    const normalized = this.normalizePath(filePath)
    if (!this.byPath.has(normalized)) return false

    const idx = this.byPath.get(normalized)
    const protectedFields = ['id', 'path', 'thumbnail']
    const cleanUpdates = { ...updates }
    for (const field of protectedFields) {
      delete cleanUpdates[field]
    }
    this.items[idx] = { ...this.items[idx], ...cleanUpdates }
    this.flush()
    return this.items[idx]
  }

  list(filter = {}) {
    let result = [...this.items]

    if (filter.type) result = result.filter(item => item.type === filter.type)
    if (filter.favorite === true) result = result.filter(item => item.favorite === true)
    if (filter.search) {
      const q = filter.search.toLowerCase()
      result = result.filter(item =>
        item.title.toLowerCase().includes(q) ||
        item.path.toLowerCase().includes(q)
      )
    }

    if (filter.sortBy === 'addedAt') result.sort((a, b) => b.addedAt.localeCompare(a.addedAt))
    else if (filter.sortBy === 'lastUsed') result.sort((a, b) => b.lastUsed.localeCompare(a.lastUsed))
    else if (filter.sortBy === 'name') result.sort((a, b) => a.title.localeCompare(b.title))

    return result
  }

  get(filePath) {
    const normalized = this.normalizePath(filePath)
    const idx = this.byPath.get(normalized)
    return idx !== undefined ? this.items[idx] : null
  }

  registerIpcHandlers() {
    ipcMain.handle('gallery.add', async (event, filePath, type, extra) => {
      return await this.add(filePath, type, extra)
    })
    ipcMain.handle('gallery.remove', (event, filePath) => this.remove(filePath))
    ipcMain.handle('gallery.update', (event, filePath, updates) =>
      this.update(filePath, updates)
    )
    ipcMain.handle('gallery.list', (event, filter) => this.list(filter || {}))
    ipcMain.handle('gallery.get', (event, filePath) => this.get(filePath))
  }
}

module.exports = { WallpaperLibrary }
