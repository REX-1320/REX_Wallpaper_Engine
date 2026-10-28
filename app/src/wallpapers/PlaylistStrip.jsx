import { useEffect, useMemo, useRef, useState } from 'react'
import { FileImage, FolderOpen, ListMusic, MonitorPlay, Trash2, X } from 'lucide-react'
import { getThumb, subscribeThumbs } from './thumbCache'
import { toFileUrl } from './thumbnail'
import { appliedMemberKey, memberKey } from './playlist/scope'
import {
  applyWallpaper,
  addPlaylistMembers,
  ensureWallpaperForPath,
  removePlaylistMember,
  reorderPlaylistMember,
  selectWallpaper,
  updateWallpaperProperty,
  useWallpaperStore,
} from './store'

const DND_MIME = 'application/x-paiint-wallpaper-paths'
const REORDER_MIME = 'application/x-paiint-playlist-reorder'

function baseName(path) {
  if (!path) return ''
  return String(path).split(/[\\/]/).pop()
}

// The playlist strip: a horizontal run of the playlist's files.
//
// Membership is keyed by the file's absolute path, so the strip resolves each
// member's name and poster from the persisted gallery library (and the shared
// thumbnail cache), not from the gallery's local file list.
export default function PlaylistStrip({ playlistId }) {
  const { playlists, wallpapers, appliedId } = useWallpaperStore()
  const playlist = playlists.find((p) => p.id === playlistId) ?? null
  const members = useMemo(() => playlist?.members ?? [], [playlist])
  // The file that is on the desktop RIGHT NOW (appliedId), so its cube can
  // be outlined. The key is the shared membership rule (case/slash
  // insensitive), so it matches the playlist's own members.
  const appliedKey = useMemo(
    () => appliedMemberKey({ wallpapers, appliedId }),
    [wallpapers, appliedId],
  )
  const [library, setLibrary] = useState({})
  const [dragOver, setDragOver] = useState(false)
  const [dragIndex, setDragIndex] = useState(null)
  const [dropIndex, setDropIndex] = useState(null)
  const [menu, setMenu] = useState(null)
  const [, bumpThumbs] = useState(0)
  // Re-render chips when the gallery or the GPU worker publishes a poster.
  useEffect(() => subscribeThumbs(() => bumpThumbs((n) => n + 1)), [])
  // The dragged cube's index lives in a ref, not just in state: the drop
  // handler must know the source even if React has not re-rendered yet.
  const dragIndexRef = useRef(null)

  useEffect(() => {
    let cancelled = false
    const pending = window.paiintDesktop?.gallery?.list?.()
    if (!pending?.then) return undefined
    pending
      .then((entries) => {
        if (cancelled) return
        const map = {}
        for (const entry of entries ?? []) map[memberKey(entry.path)] = entry
        setLibrary(map)
      })
      .catch(() => {})
    return () => {
      cancelled = true
    }
  }, [members.length, playlists.length])

  useEffect(() => {
    if (!menu) return undefined
    const close = () => setMenu(null)
    window.addEventListener('pointerdown', close)
    return () => window.removeEventListener('pointerdown', close)
  }, [menu])

  const onSelect = async (member) => {
    const id = await ensureWallpaperForPath(member)
    if (id) selectWallpaper(id)
  }

  const onApply = async (member) => {
    const id = await ensureWallpaperForPath(member)
    if (id) applyWallpaper(id, { source: 'playlist', playlist: playlist?.name })
  }

  // The playlist whose engine rules govern this file. The engine reads the
  // name (wallpaper.playlist.currentPlaylist), so this is a name, not an id.
  const setActivePlaylist = async (member, target) => {
    if (!target) return
    const id = await ensureWallpaperForPath(member)
    if (id) updateWallpaperProperty(id, ['playlist', 'currentPlaylist'], target.name)
  }

  const onDrop = (event) => {
    event.preventDefault()
    setDragOver(false)
    if (!playlistId) return
    const raw = event.dataTransfer.getData(DND_MIME)
    if (!raw) return
    try {
      const paths = JSON.parse(raw)
      if (Array.isArray(paths) && paths.length) addPlaylistMembers(playlistId, paths)
    } catch {
      /* not our payload */
    }
  }

  return (
    <div
      className={`dock-strip${dragOver ? ' drag-over' : ''}`}
      onDragOver={(e) => {
        if (!e.dataTransfer.types?.includes(DND_MIME)) return
        e.preventDefault()
        e.dataTransfer.dropEffect = 'copy'
        setDragOver(true)
      }}
      onDragLeave={() => setDragOver(false)}
      onDrop={onDrop}
    >
      {members.length === 0 ? (
        <p className="dock-strip-empty">
          {playlist ? `Drag files here to add them to ${playlist.name}` : 'No playlist selected'}
        </p>
      ) : null}

      {members.map((member, index) => {
        const key = memberKey(member)
        const entry = library[key]
        const poster = getThumb(member)?.dataUrl ?? (entry?.thumbnail ? toFileUrl(entry.thumbnail) : null)
        const name = entry?.title ?? baseName(member)
        return (
          <button
            type="button"
            key={key}
            className={`dock-chip${dropIndex === index ? ' drop-target' : ''}${dragIndex === index ? ' dragging' : ''}${key && key === appliedKey ? ' playing' : ''}`}
            title={name}
            aria-label={name}
            draggable
            onDragStart={(e) => {
              // Dragging a cube MOVES it inside the playlist (reorder). The
              // gallery's "add to playlist" payload is a different mime type,
              // so the two never collide.
              e.dataTransfer.setData(REORDER_MIME, String(index))
              e.dataTransfer.effectAllowed = 'move'
              dragIndexRef.current = index
              setDragIndex(index)
            }}
            onDragEnd={() => {
              dragIndexRef.current = null
              setDragIndex(null)
              setDropIndex(null)
            }}
            onDragOver={(e) => {
              if (dragIndexRef.current == null) return
              e.preventDefault()
              e.stopPropagation()
              e.dataTransfer.dropEffect = 'move'
              setDropIndex(index)
            }}
            onDrop={(e) => {
              const from = dragIndexRef.current
              if (from == null) return
              e.preventDefault()
              e.stopPropagation()
              reorderPlaylistMember(playlistId, from, index)
              dragIndexRef.current = null
              setDragIndex(null)
              setDropIndex(null)
            }}
            onClick={() => onSelect(member)}
            onDoubleClick={() => onApply(member)}
            onContextMenu={(e) => {
              e.preventDefault()
              setMenu({ x: e.clientX, y: e.clientY, member })
            }}
          >
            <span className="dock-chip-thumb">
              {poster ? <img src={poster} alt="" /> : <FileImage size={16} />}
            </span>
            {/* A real <button> inside the cube's <button> is invalid HTML and
                Chromium closes the outer button — so this is a span. */}
            <span
              className="dock-chip-remove"
              role="button"
              tabIndex={-1}
              aria-label="Remove from playlist"
              title="Remove from playlist"
              onPointerDown={(e) => e.stopPropagation()}
              onDragStart={(e) => e.preventDefault()}
              onClick={(e) => {
                e.stopPropagation()
                removePlaylistMember(playlistId, member)
              }}
            >
              <X size={12} />
            </span>
          </button>
        )
      })}

      {menu ? (
        <div
          className="playlist-context-menu"
          style={{ left: menu.x, top: menu.y }}
          onPointerDown={(e) => e.stopPropagation()}
        >
          <button
            type="button"
            className="playlist-context-item"
            onClick={() => {
              onApply(menu.member)
              setMenu(null)
            }}
          >
            <MonitorPlay size={13} />
            <span>Apply</span>
          </button>
          <button
            type="button"
            className="playlist-context-item"
            onClick={() => {
              window.paiintDesktop?.showInFolder?.(menu.member)
              setMenu(null)
            }}
          >
            <FolderOpen size={13} />
            <span>Open in Explorer</span>
          </button>
          <button
            type="button"
            className="playlist-context-item"
            disabled={!playlist}
            onClick={() => {
              // The active playlist is the one whose engine rules govern the
              // file; the engine reads it by name.
              setActivePlaylist(menu.member, playlist)
              setMenu(null)
            }}
          >
            <ListMusic size={13} />
            <span>Set as active playlist</span>
          </button>
          <div className="playlist-context-sep" />
          <button
            type="button"
            className="playlist-context-item danger"
            onClick={() => {
              removePlaylistMember(playlistId, menu.member)
              setMenu(null)
            }}
          >
            <Trash2 size={13} />
            <span>Remove from playlist</span>
          </button>
        </div>
      ) : null}
    </div>
  )
}
