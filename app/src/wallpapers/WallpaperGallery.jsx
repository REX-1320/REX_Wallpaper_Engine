import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ArrowDown, ArrowUp, Check, FolderOpen, ListMusic, ListPlus, Monitor, Pencil, Plus, Trash2, X } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { detectFileType, isSupportedWallpaperType, splitNewFiles, PLAYLIST_ICONS, TYPE_META } from './registry'
import {
  addPlaylistMembers,
  applyWallpaper,
  deleteWallpaper,
  isPlaylistMember,
  loadWallpaperFile,
  selectWallpaper,
  updateWallpaperProperty,
  useWallpaperStore,
} from './store'
import DisplaySection from './inspector/sections/DisplaySection'
import { createThumbnailFallback, createThumbnailFromUrl, deleteThumbnail, saveThumbnail, toFileUrl } from './thumbnail'
import { setThumb } from './thumbCache'
import { setLayoutState } from '../state/layout'
import { buildFileRecord, galleryAdd, galleryGet, galleryList, galleryRemove, galleryRestoreFile } from './gallery'

const DND_MIME = 'application/x-paiint-wallpaper-paths'

// Restore is a long chain of async poster decodes. Awaiting an already-
// resolved promise does not yield to the browser, so a burst of cheap/failed
// entries can keep the main thread busy for the whole chain. Yield a macrotask
// between entries so React can paint tiles/posters and the supervisor's 2 s
// heartbeat timer always gets a turn.
const yieldToMain = () => new Promise((resolve) => setTimeout(resolve, 0))

const SORT_OPTIONS = [
  { key: 'name', label: 'Name' },
  { key: 'type', label: 'Type' },
  { key: 'size', label: 'Size' },
  { key: 'date', label: 'Date' },
]

export default function WallpaperGallery() {
  const [files, setFiles] = useState([])
  const [sort, setSort] = useState({ by: 'name', dir: 'asc' })
  const [displayOpen, setDisplayOpen] = useState(false)
  const [editName, setEditName] = useState('')
  const [editingFile, setEditingFile] = useState(null)
  const [menu, setMenu] = useState(null)
  const [previewId, setPreviewId] = useState(null)
  const thumbsRef = useRef(new Map())
  const [, forceThumbs] = useState(0)
  const fileInputRef = useRef(null)
  const cubeRef = useRef(null)
  const { wallpapers, playlists, selectedId, appliedId } = useWallpaperStore()
  const selectedWallpaper = wallpapers.find((w) => w.id === selectedId) ?? null

  const getThumb = useCallback((id) => thumbsRef.current.get(id) ?? null, [])

  // The playlist dock renders outside the gallery, so posters are also kept
  // in a shared cache keyed by the file's path.
  const cacheThumb = useCallback((record) => {
    const key = record?.path ?? record?.name
    if (!key) return
    setThumb(key, { dataUrl: record.thumbDataUrl ?? null, posterPath: record.thumbPath ?? null })
  }, [])

  const startFileRename = (record) => {
    setEditingFile(record.id)
    setEditName(record.name)
    setMenu(null)
  }

  const commitFileRename = (record) => {
    const newName = editName.trim() || record.name
    if (newName !== record.name) {
      setFiles((prev) => prev.map((f) => (f === record ? { ...f, name: newName } : f)))
      const w = wallpapers.find((x) => x.info.source === record.name)
      if (w) {
        updateWallpaperProperty(w.id, ['info', 'name'], newName.replace(/\.[^.]+$/, ''))
        updateWallpaperProperty(w.id, ['info', 'source'], newName)
      }
    }
    setEditingFile(null)
  }

  // Adding a file to a playlist does two things: it joins the playlist's
  // membership (keyed by path, so a file can be in several playlists) and it
  // becomes that playlist's ACTIVE playlist for the file — the engine reads
  // `playlist.currentPlaylist` to decide which rule set governs the file.
  const addToPlaylist = async (record, playlist) => {
    setMenu(null)
    const member = record.path ?? record.name
    if (member) addPlaylistMembers(playlist.id, [member])
    let w = wallpapers.find((x) => x.info.source === record.name)
    let id = w?.id
    if (!id) id = await loadWallpaperFile(record.file, record.path ?? null)
    if (id) updateWallpaperProperty(id, ['playlist', 'currentPlaylist'], playlist.name)
  }

  const removeFile = async (record) => {
    const w = wallpapers.find((x) => x.info.source === record.name)
    if (w) {
      if (w.id === selectedId) selectWallpaper(null)
      deleteWallpaper(w.id)
    }
    // Drop the persisted cached poster (best-effort; bridge may be absent).
    if (record.thumbPath) {
      deleteThumbnail(record.thumbPath)?.catch?.(() => {})
    }
    // Drop the persisted library entry (best-effort). Importing the same
    // path again later re-creates both the entry and the tile.
    // P0-6: also drop the library-persisted cached poster. deleteThumbnail
    // is main-side guarded to the thumbnails cache dir, so a static entry
    // whose thumbnail is the source file itself is never touched.
    try {
      const rp =
        (window.paiintDesktop?.getPathForFile ? await window.paiintDesktop.getPathForFile(record.file) : null) ??
        null
      if (rp) {
        const entry = await galleryGet(rp).catch(() => null)
        galleryRemove(rp)?.catch?.(() => {})
        if (entry?.thumbnail) deleteThumbnail(entry.thumbnail)?.catch?.(() => {})
      }
    } catch {
      // memory-only file: nothing persisted
    }
    thumbsRef.current.delete(record.id)
    setFiles((prev) => prev.filter((f) => f !== record))
    setMenu(null)
  }

  const openInExplorer = async (record) => {
    setMenu(null)
    try {
      const path =
        (window.paiintDesktop?.getPathForFile ? await window.paiintDesktop.getPathForFile(record.file) : null) ??
        record.filePath ??
        null
      if (path) window.paiintDesktop?.showInFolder?.(path)
    } catch {
      // no-op outside desktop
    }
  }

  // Restore persisted gallery tiles after a restart (P0.3). Rebuilds File
  // objects via the gallery.read-file bridge and the same record builder as
  // fresh imports. Missing/unreadable files are skipped (kept in the library,
  // never silently pruned — the drive may be temporarily unavailable). Tiles
  // are restored only; applying stays with boot re-apply (startup.cjs).
  useEffect(() => {
    let cancelled = false
    ;(async () => {
      const items = await galleryList({ sortBy: 'lastUsed' })
      if (cancelled || !items || items.length === 0) return
      const restored = []
      for (const item of items.slice(0, 100)) {
        const { file, path: itemPath, url, missing } =
          await galleryRestoreFile(item)
        if (cancelled) return
        if (missing || !file) continue
        try {
          // Metadata only for now. Posters are decoded AFTER the tiles are on
          // screen (see below): restoring 11 library entries means 11 video
          // seeks, and doing that inline delays the first paint and competes
          // with the wallpaper's first seconds of playback — which is exactly
          // the window that already stutters while Chromium warms up.
          const record = await buildFileRecord(file, itemPath ?? item?.path ?? null, url, {
            skipThumbnail: true,
          })
          // P0-6: keep the library entry alongside so the persisted cached
          // poster can be preferred below (restored skips missing files, so
          // positional indexing into `items` would misalign).
          record.libraryItem = item
          restored.push(record)
        } catch {
          // unloadable file: skip tile, keep library entry
        }
      }
      // Restored tiles are appended even when none produced a thumbnail; a
      // poster-less tile is still a usable, clickable entry.
      if (cancelled || restored.length === 0) return
      // P0-6: prefer the persisted cached poster (no re-decode). The cache
      // file may be gone (user cleaned the cache dir) — probe it with an
      // Image first and fall through to progressive decode on failure, so a
      // stale library path self-heals instead of showing a broken image.
      const probePoster = (fileUrl) =>
        new Promise((resolve) => {
          if (!fileUrl) return resolve(null)
          const img = new Image()
          let timer = null
          const done = (value) => {
            if (timer) clearTimeout(timer)
            img.onload = null
            img.onerror = null
            // Abort a failed/timed-out probe so it cannot pin the file handle.
            // On success keep the decoded image (the tile reuses the same URL);
            // clearing src here would only fire a spurious error.
            if (!value) {
              try {
                img.src = ''
              } catch { /* already detached */ }
            }
            resolve(value)
          }
          // A library path can point at a slow/offline drive. Without a
          // timeout one probe would wedge the restore loop and every poster
          // after it would never appear.
          timer = setTimeout(() => done(null), 1500)
          img.onload = () => done(fileUrl)
          img.onerror = () => done(null)
          img.src = fileUrl
        })
      for (const r of restored) {
        if (cancelled || r.thumbDataUrl) continue
        const cached = r.libraryItem?.thumbnail
          ? await probePoster(toFileUrl(r.libraryItem.thumbnail))
          : null
        if (cancelled) return
        if (cached) r.thumbDataUrl = cached
        await yieldToMain()
      }
      for (const r of restored) {
        if (r.thumbDataUrl) {
          thumbsRef.current.set(r.id, r.thumbDataUrl)
          cacheThumb(r)
        }
      }
      forceThumbs((n) => n + 1)
      setFiles((prev) => [...prev, ...restored])
      // Let React commit/paint the restored tiles before any poster decode.
      await yieldToMain()

      // Posters, progressively, off the critical path.
      for (const r of restored) {
        if (cancelled || r.thumbDataUrl || !r.thumbUrl) continue
        try {
          let thumb = await createThumbnailFromUrl(
            r.thumbUrl,
            r.isVideo,
          )
          let fallbackPath = null
          if (!thumb && r.isVideo && r.path) {
            // Software-composited Chromium has no HEVC decoder; the main
            // process decodes one frame in a short-lived GPU worker instead.
            const fallback = await createThumbnailFallback(r.path, 480)
            if (fallback) {
              thumb = fallback.dataUrl
              fallbackPath = fallback.path
            }
          }
          if (cancelled || !thumb) continue
          r.thumbDataUrl = thumb
          if (fallbackPath) r.thumbPath = fallbackPath
          thumbsRef.current.set(r.id, thumb)
          cacheThumb(r)
          forceThumbs((n) => n + 1)
          // Persist the freshly decoded poster. Older library entries (and
          // entries whose cache file was cleaned) carry thumbnail: '' and
          // would otherwise seek into the source video on EVERY launch --
          // 4K decoders, tens of MB of VRAM each, plus slow startup.
          if (r.path && r.libraryItem && !r.libraryItem.thumbnail) {
            let thumbPath = fallbackPath
            if (!thumbPath) {
              try {
                thumbPath = await saveThumbnail(r.id, thumb)
              } catch {
                thumbPath = null
              }
            }
            if (thumbPath) {
              try {
                r.thumbPath = thumbPath
                r.libraryItem.thumbnail = thumbPath
                await galleryAdd(
                  r.path,
                  r.libraryItem.type ?? (r.isVideo ? 'video' : 'auto'),
                  { thumbnail: thumbPath },
                )
              } catch {
                // poster cache is best-effort
              }
            }
          }
        } catch {
          // poster failed: the tile stays, just without an image
        }
        // One poster at a time (bounded concurrency) and a real yield between
        // entries: canvas.toDataURL is synchronous, so back-to-back decodes
        // must not monopolise the main thread.
        await yieldToMain()
      }
    })().catch(() => {})
    return () => {
      cancelled = true
    }
  }, [])

  useEffect(() => {
    return () => {
      for (const url of thumbsRef.current.values()) {
        if (url?.startsWith('blob:')) URL.revokeObjectURL(url)
      }
      thumbsRef.current.clear()
    }
  }, [])

  useEffect(() => {
    if (!displayOpen) return
    const onPointerDown = (e) => {
      const target = e.target
      if (target.closest?.('[data-radix-popper-content-wrapper]') || target.closest?.('.playlist-context-menu')) return
      if (cubeRef.current && !cubeRef.current.contains(target)) setDisplayOpen(false)
    }
    document.addEventListener('pointerdown', onPointerDown)
    return () => document.removeEventListener('pointerdown', onPointerDown)
  }, [displayOpen])

  useEffect(() => {
    if (!menu) return
    const onPointerDown = () => setMenu(null)
    const onKeyDown = (e) => {
      if (e.key === 'Escape') setMenu(null)
    }
    document.addEventListener('pointerdown', onPointerDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('pointerdown', onPointerDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [menu])

  const handleFiles = async (e) => {
    const raw = [...e.target.files]
    if (raw.length === 0) return
    // Real paths for persisted-library dedup (P0.3). The library is
    // authoritative for path identity across restarts; splitNewFiles below
    // stays as the same-session exact-bytes (name+size+mtime) preflight.
    const realPaths = new Map()
    await Promise.all(
      raw.map(async (f) => {
        try {
          const p =
            (window.paiintDesktop?.getPathForFile ? await window.paiintDesktop.getPathForFile(f) : null) ??
            f.path ??
            null
          if (p) realPaths.set(f, p)
        } catch {
          // memory-only file: in-memory dedup only
        }
      }),
    )
    const persistedDupes = []
    const candidates = []
    for (const f of raw) {
      const p = realPaths.get(f)
      const hit = p ? await galleryGet(p) : null
      if (hit) persistedDupes.push(f)
      else candidates.push(f)
    }
    // Duplicate guard: re-adding the same file selects the existing
    // wallpaper instead of creating a second tile + store entry.
    const { fresh: freshSplit, dupes } = splitNewFiles(files, candidates)
    const allDupes = [...persistedDupes, ...dupes]
    // All dupes: select the live tile when there is one. A persisted dupe
    // with no live tile (e.g. restore skipped it) is imported fresh so the
    // user still gets a tile; the library bumps lastUsed, no duplicate entry.
    let freshList = freshSplit
    if (freshList.length === 0) {
      const first = allDupes[0]
      const existing = first && wallpapers.find((w) => w.info.source === first.name)
      if (existing) {
        // Re-importing a known file is an explicit action: apply it.
        applyWallpaper(existing.id)
        e.target.value = ''
        return
      }
      if (!first) {
        e.target.value = ''
        return
      }
      freshList = [first]
    }
    const records = await Promise.all(freshList.map((f) => buildFileRecord(f, realPaths.get(f) ?? null)))
    for (const r of records) {
      if (r.thumbDataUrl) {
        thumbsRef.current.set(r.id, r.thumbDataUrl)
        cacheThumb(r)
      }
      // These files are new to the library, so the Settings page's defaults
      // apply to them (a file that was already in the library keeps its own
      // values).
      r.isNewImport = true
    }
    forceThumbs((n) => n + 1)
    setFiles((prev) => [...prev, ...records])
    // Persist fresh imports to the library (best-effort; bridge absent
    // outside the desktop app). gallery.add dedups by normalized path, so a
    // re-imported edited file bumps lastUsed instead of duplicating.
    // P0-6: hand the freshly persisted poster to the library so a later
    // restart can prefer the cached webp over re-decoding the video.
    freshList.forEach((f, i) => {
      const p = realPaths.get(f)
      if (p) {
        const extra = records[i]?.thumbPath ? { thumbnail: records[i].thumbPath } : undefined
        galleryAdd(p, f.type.startsWith('video/') ? 'video' : 'auto', extra).catch(() => {})
      }
    })
    const firstId = await loadWallpaperFile(records[0].file, records[0].path ?? null, { isNew: true })
    if (firstId) applyWallpaper(firstId)
    e.target.value = ''
  }

  // Single click previews (select), double click applies. Both paths share
  // one in-flight load per tile so a fast double click cannot import the
  // same file twice.
  const pendingLoads = useRef(new Map())
  const ensureLoaded = useCallback((record) => {
    const existing = wallpapers.find((w) => w.info.source === record.name)
    if (existing) return Promise.resolve(existing.id)
    const pending = pendingLoads.current.get(record.id)
    if (pending) return pending
    const p = loadWallpaperFile(record.file, record.path ?? null, { isNew: !!record.isNewImport }).finally(() => {
      pendingLoads.current.delete(record.id)
    })
    pendingLoads.current.set(record.id, p)
    return p
  }, [wallpapers])

  const handleSelect = async (record) => {
    const id = await ensureLoaded(record)
    if (id) selectWallpaper(id)
  }

  const handleApply = async (record) => {
    const id = await ensureLoaded(record)
    if (id) applyWallpaper(id)
  }

  const handleSort = (by) => {
    setSort((prev) => (prev.by === by ? { by, dir: prev.dir === 'asc' ? 'desc' : 'asc' } : { by, dir: 'asc' }))
  }

  const sortedFiles = useMemo(() => {
    const dir = sort.dir === 'asc' ? 1 : -1
    return [...files].sort((a, b) => {
      let cmp = 0
      if (sort.by === 'name') cmp = a.name.localeCompare(b.name)
      else if (sort.by === 'type') cmp = (detectFileType(a.name) ?? '').localeCompare(detectFileType(b.name) ?? '')
      else if (sort.by === 'size') cmp = a.size - b.size
      else if (sort.by === 'date') cmp = a.date - b.date
      return cmp * dir
    })
  }, [files, sort])

  return (
    <div className="wallpaper-gallery">
      <div className="wallpaper-gallery-head">
        <span className="wallpaper-gallery-title">
          Gallery
          <span className="wallpaper-gallery-count">
            {files.length} {files.length === 1 ? 'wallpaper' : 'wallpapers'}
          </span>
        </span>
        <div className="wallpaper-gallery-actions">
          <div className="display-cube-wrap">
              <motion.div
                ref={cubeRef}
                className={`display-cube${displayOpen ? ' open' : ''}`}
                initial={false}
                animate={{
                  width: displayOpen ? 300 : 104,
                  height: displayOpen ? 420 : 28,
                  borderRadius: displayOpen ? 18 : 999,
                }}
                transition={{ type: 'spring', stiffness: 220, damping: 26, mass: 1 }}
              >
                <motion.div
                  className="display-cube-bg"
                  initial={false}
                  animate={{ opacity: displayOpen ? 1 : 0 }}
                  transition={{ duration: 0.35, ease: [0.22, 1, 0.36, 1] }}
                />
                <button
                  type="button"
                  className="display-cube-head"
                  aria-label="Toggle display settings"
                  onClick={() => setDisplayOpen((v) => !v)}
                >
                  {displayOpen ? <X size={14} /> : null}
                  <span className="display-cube-title">
                    <Monitor size={14} />
                    <span>Display</span>
                  </span>
                </button>
                {displayOpen ? (
                  <motion.div
                    initial={{ opacity: 0, y: 6 }}
                    animate={{ opacity: 1, y: 0 }}
                    transition={{ duration: 0.25, ease: [0.22, 1, 0.36, 1] }}
                    className="display-cube-content"
                  >
                    {selectedWallpaper ? (
                      <DisplaySection
                        wallpaper={selectedWallpaper}
                        inline
                        applied={selectedWallpaper.id === appliedId}
                      />
                    ) : (
                      <p className="display-settings-hint">
                        Select a wallpaper in the gallery to edit its display settings.
                      </p>
                    )}
                  </motion.div>
                ) : null}
              </motion.div>
            </div>
            <button
              type="button"
              className="new-pill"
              aria-label="Import wallpapers"
              onClick={() => fileInputRef.current?.click()}
            >
              <Plus size={14} />
              <span>New</span>
            </button>
        </div>
        <input
          ref={fileInputRef}
          type="file"
          hidden
          multiple
          accept=".png,.jpg,.jpeg,.bmp,.tif,.tiff,.mp4,.mkv,.avi,.mov,.wmv,.m4v,.webm,.mpg,.mpeg"
          onChange={handleFiles}
        />
      </div>

      <div className="wallpaper-gallery-toolbar">
        <DropdownMenu>
          <DropdownMenuTrigger asChild>
            <button type="button" className="new-pill toolbar-pill" aria-label="Sort gallery">
              {sort.dir === 'asc' ? <ArrowUp size={14} /> : <ArrowDown size={14} />}
              <span>Sort</span>
            </button>
          </DropdownMenuTrigger>
          <DropdownMenuContent align="start" className="w-40">
            <DropdownMenuLabel>Sort by</DropdownMenuLabel>
            <DropdownMenuSeparator />
            {SORT_OPTIONS.map(({ key, label }) => (
              <DropdownMenuItem key={key} onClick={() => handleSort(key)}>
                {sort.by === key ? (
                  sort.dir === 'asc' ? <ArrowUp size={14} /> : <ArrowDown size={14} />
                ) : (
                  <span className="size-3.5" />
                )}
                {label}
              </DropdownMenuItem>
            ))}
          </DropdownMenuContent>
        </DropdownMenu>

        <button
          type="button"
          className="new-pill toolbar-pill"
          style={{ marginLeft: 'auto' }}
          aria-label="Open playlist panel"
          onClick={() => setLayoutState({ panelOpen: true })}
        >
          <ListMusic size={14} />
          <span>Playlist</span>
        </button>
      </div>

      <div className="gallery-grid">
        <AnimatePresence>
          {sortedFiles.map((record) => {
            const { name } = record
            const type = detectFileType(name) ?? 'IMAGE'
            const meta = TYPE_META[type]
            const Icon = meta.icon
            const supported = isSupportedWallpaperType(type)
            const selected = wallpapers.some((w) => w.info.source === name && w.id === selectedId) || previewId === record.id
            return (
              <motion.button
                type="button"
                key={record.id}
                layout
                draggable={supported}
                initial={{ opacity: 0, scale: 0.9 }}
                animate={{ opacity: 1, scale: 1 }}
                exit={{ opacity: 0, scale: 0.92 }}
                transition={{ type: 'spring', stiffness: 320, damping: 28 }}
                className={`gallery-tile${selected ? ' selected' : ''}`}
                title={supported ? `${name} — double-click to apply` : `${name} — ${meta.label} wallpapers are coming soon`}
                style={{ '--file-gradient': meta.gradient }}
                onDragStart={(e) => {
                  // The playlist dock is the drop target. Membership is keyed by
                  // the file's path; without a bridge (browser dev) the name is
                  // the only stable identity available.
                  const member = record.path ?? record.name
                  if (!member) return
                  e.dataTransfer.setData(DND_MIME, JSON.stringify([member]))
                  e.dataTransfer.effectAllowed = 'copy'
                }}
                onClick={() => {
                  if (supported) handleSelect(record)
                  else setPreviewId(record.id)
                }}
                onDoubleClick={() => {
                  if (supported) handleApply(record)
                }}
                onContextMenu={(e) => {
                  e.preventDefault()
                  setMenu({
                    x: Math.min(e.clientX, window.innerWidth - 196),
                    y: Math.min(e.clientY, window.innerHeight - 240),
                    record,
                    addOpen: false,
                  })
                }}
              >
                {getThumb(record.id) ? (
                  <span className="gallery-tile-media">
                    <img src={getThumb(record.id)} alt={name} />
                  </span>
                ) : (
                  <span className="gallery-tile-thumb">
                    <Icon size={30} />
                  </span>
                )}
                {!supported ? (
                  <span
                    className="gallery-tile-badge"
                    style={{
                      position: 'absolute',
                      top: 6,
                      right: 6,
                      fontSize: 10,
                      padding: '2px 6px',
                      borderRadius: 999,
                      background: 'rgba(0,0,0,0.65)',
                      color: '#ffd479',
                    }}
                  >
                    Coming soon
                  </span>
                ) : null}
                {editingFile === record.id ? (
                  <span className="gallery-tile-rename-wrap">
                    <input
                      autoFocus
                      draggable={false}
                      className="gallery-tile-rename"
                      value={editName}
                      onChange={(e) => setEditName(e.target.value)}
                      onBlur={() => commitFileRename(record)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') commitFileRename(record)
                        if (e.key === 'Escape') setEditingFile(null)
                      }}
                      onClick={(e) => e.stopPropagation()}
                      onPointerDown={(e) => e.stopPropagation()}
                      onDragStart={(e) => e.preventDefault()}
                    />
                  </span>
                ) : (
                  <span className="gallery-tile-name">{name}</span>
                )}
                <span className="gallery-tile-type">{meta.label}</span>
              </motion.button>
            )
          })}
        </AnimatePresence>
        {files.length === 0 ? (
          <div className="gallery-empty">
            <FolderOpen size={22} />
            <p>
              No wallpapers yet.
              <br />
              Click New to import files.
            </p>
          </div>
        ) : null}
      </div>

      {menu ? (
        <div
          className="playlist-context-menu"
          style={{ left: menu.x, top: menu.y }}
          onPointerDown={(e) => e.stopPropagation()}
          onContextMenu={(e) => {
            e.preventDefault()
            e.stopPropagation()
          }}
        >
          <div
            className="playlist-context-addwrap"
            onMouseEnter={() => setMenu((m) => (m ? { ...m, addOpen: true } : m))}
            onMouseLeave={() => setMenu((m) => (m ? { ...m, addOpen: false } : m))}
          >
            <button
              type="button"
              className={`playlist-context-item${menu.addOpen ? ' open' : ''}`}
              onClick={() => setMenu((m) => (m ? { ...m, addOpen: !m.addOpen } : m))}
            >
              <ListPlus size={13} />
              <span>Add to playlist</span>
            </button>
            {menu.addOpen ? (
              <div
                className="playlist-context-submenu"
                style={{ right: menu.x > window.innerWidth - 380 ? '100%' : 'auto' }}
              >
                {playlists
                  .filter((pl) => pl.name !== 'None')
                  .map((pl) => {
                    const Icon = PLAYLIST_ICONS[pl.icon] ?? ListMusic
                    const member = menu.record.path ?? menu.record.name
                    const isMember = isPlaylistMember(pl.id, member)
                    return (
                      <button
                        type="button"
                        key={pl.id}
                        className="playlist-context-item"
                        onClick={() => addToPlaylist(menu.record, pl)}
                      >
                        <Icon size={13} />
                        <span>{pl.name}</span>
                        {isMember ? <Check size={13} /> : null}
                      </button>
                    )
                  })}
                {playlists.filter((pl) => pl.name !== 'None').length === 0 ? (
                  <span className="playlist-context-label">No playlists</span>
                ) : null}
              </div>
            ) : null}
          </div>
          <div className="playlist-context-sep" />
          <button
            type="button"
            className="playlist-context-item"
            onClick={() => startFileRename(menu.record)}
          >
            <Pencil size={13} />
            <span>Rename</span>
          </button>
          <button
            type="button"
            className="playlist-context-item danger"
            onClick={() => removeFile(menu.record)}
          >
            <Trash2 size={13} />
            <span>Remove</span>
          </button>
          <div className="playlist-context-sep" />
          <button
            type="button"
            className="playlist-context-item"
            onClick={() => openInExplorer(menu.record)}
          >
            <FolderOpen size={13} />
            <span>Open in Explorer</span>
          </button>
        </div>
      ) : null}

    </div>
  )
}