import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ArrowDown, ArrowUp, Check, Folder, FolderOpen, ListMusic, ListPlus, Monitor, Pencil, Plus, Settings2, Trash2, X } from 'lucide-react'
import { AnimatePresence, motion } from 'motion/react'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { detectFileType, isSupportedWallpaperType, splitNewFiles, PLAYLIST_ICON_CHOICES, PLAYLIST_ICONS, TYPE_META } from './registry'
import {
  deletePlaylist,
  deleteWallpaper,
  loadWallpaperFile,
  renamePlaylist,
  selectWallpaper,
  setPlaylistIcon,
  updateWallpaperProperty,
  useWallpaperStore,
} from './store'
import DisplaySection from './inspector/sections/DisplaySection'
import PlaylistEngineDialog from './playlist/PlaylistEngineDialog'
import { playlistEngine } from './playlist/engine'
import { createThumbnail, deleteThumbnail, saveThumbnail } from './thumbnail'

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
  const [playlistOpen, setPlaylistOpen] = useState(false)
  const [folders, setFolders] = useState([])
  const [editingId, setEditingId] = useState(null)
  const [editName, setEditName] = useState('')
  const [editingFile, setEditingFile] = useState(null)
  const [menu, setMenu] = useState(null)
  const [previewId, setPreviewId] = useState(null)
  const [settingsTarget, setSettingsTarget] = useState(null)
  const thumbsRef = useRef(new Map())
  const [, forceThumbs] = useState(0)
  const fileInputRef = useRef(null)
  const cubeRef = useRef(null)
  const playlistCubeRef = useRef(null)
  const { wallpapers, playlists, selectedId } = useWallpaperStore()
  const selectedWallpaper = wallpapers.find((w) => w.id === selectedId) ?? null

  const getThumb = useCallback((id) => thumbsRef.current.get(id) ?? null, [])

  const startRename = (folder) => {
    setEditingId(folder.id)
    setEditName(folder.name)
  }

  const createFolder = () => {
    const base = 'New folder'
    let name = base
    let i = 2
    while (folders.some((f) => f.name === name)) name = `${base} (${i++})`
    const id = crypto.randomUUID()
    setFolders((prev) => [...prev, { id, name }])
    setEditingId(id)
    setEditName(name)
  }

  const commitRename = (target) => {
    const name = editName.trim() || target.name
    if (target.id.startsWith('pl-')) renamePlaylist(target.id, name)
    else setFolders((prev) => prev.map((f) => (f.id === target.id ? { ...f, name } : f)))
    setEditingId(null)
  }

  const cancelRename = () => setEditingId(null)

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

  const addToPlaylist = async (record, playlistName) => {
    setMenu(null)
    let w = wallpapers.find((x) => x.info.source === record.name)
    let id = w?.id
    if (!id) id = await loadWallpaperFile(record.file)
    if (id) updateWallpaperProperty(id, ['playlist', 'currentPlaylist'], playlistName)
  }

  const removeFile = (record) => {
    const w = wallpapers.find((x) => x.info.source === record.name)
    if (w) {
      if (w.id === selectedId) selectWallpaper(null)
      deleteWallpaper(w.id)
    }
    // Drop the persisted cached poster (best-effort; bridge may be absent).
    if (record.thumbPath) {
      deleteThumbnail(record.thumbPath)?.catch?.(() => {})
    }
    thumbsRef.current.delete(record.id)
    setFiles((prev) => prev.filter((f) => f !== record))
    setMenu(null)
  }

  const openInExplorer = async (record) => {
    setMenu(null)
    try {
      const path =
        (window.rexDesktop?.getPathForFile ? await window.rexDesktop.getPathForFile(record.file) : null) ??
        record.filePath ??
        null
      if (path) window.rexDesktop?.showInFolder?.(path)
    } catch {
      // no-op outside desktop
    }
  }

  useEffect(() => {
    playlistEngine.start()
    return () => playlistEngine.stop()
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
    if (!displayOpen && !playlistOpen) return
    const onPointerDown = (e) => {
      const target = e.target
      if (target.closest?.('[data-radix-popper-content-wrapper]') || target.closest?.('.playlist-context-menu')) return
      if (displayOpen && cubeRef.current && !cubeRef.current.contains(target)) setDisplayOpen(false)
      if (playlistOpen && playlistCubeRef.current && !playlistCubeRef.current.contains(target)) {
        setPlaylistOpen(false)
      }
    }
    document.addEventListener('pointerdown', onPointerDown)
    return () => document.removeEventListener('pointerdown', onPointerDown)
  }, [displayOpen, playlistOpen])

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
    // Duplicate guard: re-adding the same file selects the existing
    // wallpaper instead of creating a second tile + store entry.
    const { fresh, dupes } = splitNewFiles(files, raw)
    if (fresh.length === 0) {
      const first = dupes[0]
      const existing = first && wallpapers.find((w) => w.info.source === first.name)
      if (existing) selectWallpaper(existing.id)
      e.target.value = ''
      return
    }
    const records = await Promise.all(
      fresh.map(async (f) => {
        const thumb = await createThumbnail(f)
        const id = crypto.randomUUID()
        const thumbDataUrl = thumb?.dataUrl ?? null
        // Persist the poster to the REX thumbnail cache so tiles survive
        // past the in-memory blob URL lifetime. Best-effort: the bridge
        // is absent outside the desktop app.
        let thumbPath = null
        if (thumbDataUrl) {
          try {
            thumbPath = await saveThumbnail(id, thumbDataUrl)
          } catch {
            thumbPath = null
          }
        }
        return {
          id,
          name: f.name,
          size: f.size,
          date: f.lastModified,
          file: f,
          thumbDataUrl,
          thumbPath,
          isVideo: f.type.startsWith('video/'),
        }
      }),
    )
    for (const r of records) {
      if (r.thumbDataUrl) thumbsRef.current.set(r.id, r.thumbDataUrl)
    }
    forceThumbs((n) => n + 1)
    setFiles((prev) => [...prev, ...records])
    await loadWallpaperFile(records[0].file)
    e.target.value = ''
  }

  const handleOpen = async (record) => {
    const existing = wallpapers.find((w) => w.info.source === record.name)
    if (existing) selectWallpaper(existing.id)
    else await loadWallpaperFile(record.file)
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
        <span className="wallpaper-gallery-title">Gallery</span>
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
                      <DisplaySection wallpaper={selectedWallpaper} expandable={false} />
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

        <div className="playlist-cube-wrap" style={{ marginLeft: 'auto' }}>
          <motion.div
            ref={playlistCubeRef}
            className={`playlist-cube${playlistOpen ? ' open' : ''}`}
            initial={false}
            animate={{
              width: playlistOpen ? 300 : 104,
              height: playlistOpen ? 400 : 28,
              borderRadius: playlistOpen ? 18 : 999,
            }}
            transition={{ type: 'spring', stiffness: 220, damping: 26, mass: 1 }}
          >
            <motion.div
              className="playlist-cube-bg"
              initial={false}
              animate={{ opacity: playlistOpen ? 1 : 0 }}
              transition={{ duration: 0.35, ease: [0.22, 1, 0.36, 1] }}
            />
            <div className="playlist-cube-head">
              <div className="playlist-cube-tools">
                <button
                  type="button"
                  className="playlist-cube-new"
                  aria-label="New folder"
                  onClick={createFolder}
                >
                  <Plus size={13} />
                </button>
                <button
                  type="button"
                  className="playlist-cube-new"
                  aria-label="Playlist settings"
                  onClick={() => setSettingsTarget('active')}
                >
                  <Settings2 size={13} />
                </button>
              </div>
              <button
                type="button"
                className="playlist-cube-toggle"
                aria-label="Toggle playlist"
                onClick={() => setPlaylistOpen((v) => !v)}
              >
                <span className="playlist-cube-title">
                  <ListMusic size={14} />
                  <span>Playlist</span>
                </span>
              </button>
            </div>
            {playlistOpen ? (
              <motion.div
                initial={{ opacity: 0, y: 6 }}
                animate={{ opacity: 1, y: 0 }}
                transition={{ duration: 0.25, ease: [0.22, 1, 0.36, 1] }}
                className="playlist-cube-content"
              >
                {folders.map((folder) =>
                  editingId === folder.id ? (
                    <input
                      key={folder.id}
                      autoFocus
                      className="playlist-cube-folder-input"
                      value={editName}
                      onChange={(e) => setEditName(e.target.value)}
                      onBlur={() => commitRename(folder)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') commitRename(folder)
                        if (e.key === 'Escape') cancelRename()
                      }}
                      onPointerDown={(e) => e.stopPropagation()}
                    />
                  ) : (
                    <button
                      type="button"
                      key={folder.id}
                      className="playlist-cube-item"
                      onClick={() => startRename(folder)}
                    >
                      <Folder size={13} />
                      <span>{folder.name}</span>
                    </button>
                  ),
                )}
                {playlists.map((playlist) => {
                  const Icon = PLAYLIST_ICONS[playlist.icon] ?? ListMusic
                  const selected = selectedWallpaper?.playlist.currentPlaylist === playlist.name
                  return editingId === playlist.id ? (
                    <input
                      key={playlist.id}
                      autoFocus
                      className="playlist-cube-folder-input"
                      value={editName}
                      onChange={(e) => setEditName(e.target.value)}
                      onBlur={() => commitRename(playlist)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') commitRename(playlist)
                        if (e.key === 'Escape') cancelRename()
                      }}
                      onPointerDown={(e) => e.stopPropagation()}
                    />
                  ) : (
                    <button
                      type="button"
                      key={playlist.id}
                      className={`playlist-cube-item${selected ? ' selected' : ''}`}
                      disabled={!selectedWallpaper}
                      onClick={() => {
                        updateWallpaperProperty(selectedWallpaper.id, ['playlist', 'currentPlaylist'], playlist.name)
                      }}
                      onContextMenu={(e) => {
                        e.preventDefault()
                        setMenu({
                          x: Math.min(e.clientX, window.innerWidth - 196),
                          y: Math.min(e.clientY, window.innerHeight - 250),
                          playlist,
                        })
                      }}
                    >
                      <Icon size={13} />
                      <span>{playlist.name}</span>
                      {selected ? <Check size={13} /> : null}
                    </button>
                  )
                })}
                {!selectedWallpaper ? (
                  <p className="playlist-cube-hint">Select a wallpaper first.</p>
                ) : null}
              </motion.div>
            ) : null}
          </motion.div>
        </div>
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
                initial={{ opacity: 0, scale: 0.9 }}
                animate={{ opacity: 1, scale: 1 }}
                exit={{ opacity: 0, scale: 0.92 }}
                transition={{ type: 'spring', stiffness: 320, damping: 28 }}
                className={`gallery-tile${selected ? ' selected' : ''}`}
                title={supported ? `${name} — click to load` : `${name} — ${meta.label} wallpapers are coming soon`}
                style={{ '--file-gradient': meta.gradient }}
                onClick={() => {
                  if (supported) handleOpen(record)
                  else setPreviewId(record.id)
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
          {menu.playlist ? (
            <>
              <button
                type="button"
                className="playlist-context-item"
                onClick={() => {
                  setSettingsTarget(menu.playlist.id)
                  setMenu(null)
                }}
              >
                <Settings2 size={13} />
                <span>Settings</span>
              </button>
              <button
                type="button"
                className="playlist-context-item"
                onClick={() => {
                  startRename(menu.playlist)
                  setMenu(null)
                }}
              >
                <Pencil size={13} />
                <span>Rename</span>
              </button>
              <div className="playlist-context-label">Icon</div>
              <div className="playlist-context-icons">
                {PLAYLIST_ICON_CHOICES.map((key) => {
                  const Icon = PLAYLIST_ICONS[key]
                  return (
                    <button
                      type="button"
                      key={key}
                      className={`playlist-context-icon${menu.playlist.icon === key ? ' active' : ''}`}
                      aria-label={key}
                      onClick={() => {
                        setPlaylistIcon(menu.playlist.id, key)
                        setMenu(null)
                      }}
                    >
                      <Icon size={14} />
                    </button>
                  )
                })}
              </div>
              <div className="playlist-context-sep" />
              <button
                type="button"
                className="playlist-context-item danger"
                onClick={() => {
                  deletePlaylist(menu.playlist.id)
                  setMenu(null)
                }}
              >
                <Trash2 size={13} />
                <span>Delete</span>
              </button>
            </>
          ) : (
            <>
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
                    {playlists.map((pl) => {
                      const Icon = PLAYLIST_ICONS[pl.icon] ?? ListMusic
                      const w = wallpapers.find((x) => x.info.source === menu.record.name)
                      return (
                        <button
                          type="button"
                          key={pl.id}
                          className="playlist-context-item"
                          onClick={() => addToPlaylist(menu.record, pl.name)}
                        >
                          <Icon size={13} />
                          <span>{pl.name}</span>
                          {w?.playlist.currentPlaylist === pl.name ? <Check size={13} /> : null}
                        </button>
                      )
                    })}
                    {playlists.length === 0 ? (
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
            </>
          )}
        </div>
      ) : null}

      {settingsTarget ? (
        <PlaylistEngineDialog
          initialPlaylistId={settingsTarget === 'active' ? null : settingsTarget}
          onClose={() => setSettingsTarget(null)}
        />
      ) : null}
    </div>
  )
}