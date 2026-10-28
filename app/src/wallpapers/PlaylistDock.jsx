import { useEffect, useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import { ChevronDown, ListMusic, Minus, Pencil, Plus, Square, Trash2, X } from 'lucide-react'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuSub,
  DropdownMenuSubContent,
  DropdownMenuSubTrigger,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import ResizeHandle from '../components/ResizeHandle'
import PlaylistStrip from './PlaylistStrip'
import PlaylistEngineDialog from './playlist/PlaylistEngineDialog'
import { playlistEngine } from './playlist/engine'
import { PLAYLIST_ICON_CHOICES, PLAYLIST_ICONS } from './registry'
import { createPlaylist, deletePlaylist, renamePlaylist, setPlaylistIcon, useWallpaperStore, applyWallpaper, ensureWallpaperForPath } from './store'
import { setLayoutState, useLayoutState } from '../state/layout'
import './PlaylistDock.css'

// The bottom dock (VS Code's terminal panel).
//
// It is global: it stays docked at the bottom on every page, because the
// playlist it holds is a property of the app, not of the Wallpapers page.
// The dock's own playlist selection is sticky — a drag into "Nature" must not
// retarget the drop zone because the focus moved.
export default function PlaylistDock({
  height,
  open,
  minimized,
  maximized,
  onResize,
  onCommitResize,
  onMinimize,
  onMaximize,
  onClose,
}) {
  const { playlists } = useWallpaperStore()
  const { panelPlaylistId } = useLayoutState()
  const [renaming, setRenaming] = useState(false)
  const [nameDraft, setNameDraft] = useState('')
  const [configOpen, setConfigOpen] = useState(false)

  // 'None' (pl-0) is the "no active playlist" sentinel, not a container.
  const containers = useMemo(() => playlists.filter((p) => p.name !== 'None'), [playlists])
  // The dock's playlist is sticky (a drag into "Nature" must not retarget the
  // drop zone when the focus moves). Until one is picked, show the first
  // playlist that actually holds files.
  const current =
    containers.find((p) => p.id === panelPlaylistId) ??
    containers.find((p) => p.members.length > 0) ??
    containers[0] ??
    null
  const Icon = current ? (PLAYLIST_ICONS[current.icon] ?? ListMusic) : ListMusic

  // Is THIS playlist the one governing the desktop right now?
  //
  // Ask the engine, not the wallpaper record. currentPlaylist is a runtime-only
  // tag on ONE record (the file the dock applied); the moment the desktop file
  // changes - the engine rotates, the user applies another member - that file
  // carries the default 'None', so a name comparison said the playlist had been
  // "exited" while the engine was still rotating through it (playlistFor's
  // membership fallback). playlistEngine.activePlaylist() is that same
  // resolution, so the badge and the rotation can never disagree.
  const isApplied = Boolean(current && playlistEngine.activePlaylist()?.id === current.id)

  useEffect(() => {
    if (!renaming) return
    setNameDraft(current?.name ?? '')
  }, [renaming, current?.name])

  const commitRename = () => {
    const name = nameDraft.trim()
    if (current && name && name !== current.name) renamePlaylist(current.id, name)
    setRenaming(false)
  }

  const addPlaylist = () => {
    const base = 'New playlist'
    let name = base
    let i = 2
    while (containers.some((p) => p.name === name)) name = `${base} (${i++})`
    const id = createPlaylist(name)
    setLayoutState({ panelPlaylistId: id })
  }

  const removeCurrent = () => {
    if (!current) return
    deletePlaylist(current.id)
    setLayoutState({ panelPlaylistId: null })
  }

  // "apply" = put THIS playlist on the desktop.
  //
  // Two things have to happen, in this order: the first file becomes the
  // wallpaper on the desktop, and that wallpaper's active playlist becomes
  // this one - the engine rotates through the ACTIVE playlist (the strip's
  // "Set as active playlist" writes the same property, by name, because
  // the runtime reads the name). Applying the file without switching the
  // active playlist would leave the old playlist's rotation running.
  const applyCurrent = async () => {
    if (!current || current.members.length === 0) return
    const id = await ensureWallpaperForPath(current.members[0])
    if (!id) return
    applyWallpaper(id, { source: 'playlist', playlist: current.name })
  }

  const dockHeight = !open ? 0 : maximized ? '100%' : minimized ? 36 : height

  return (
    <div className="dock" style={{ height: dockHeight }}>
      {open ? (
        <>
          {/* The resize handle is dropped while minimized: the strip is only
              36px tall, and the handle would push the header out of the
              panel (it was cropping the buttons). */}
          {!minimized ? (
            <ResizeHandle
              axis="y"
              className="dock-resize"
              label="Resize playlist panel"
              onResize={onResize}
              onCommit={onCommitResize}
            />
          ) : null}
          <div className="dock-header">
            {renaming ? (
              <input
                className="dock-rename"
                autoFocus
                value={nameDraft}
                onChange={(e) => setNameDraft(e.target.value)}
                onBlur={commitRename}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') commitRename()
                  if (e.key === 'Escape') setRenaming(false)
                }}
                onPointerDown={(e) => e.stopPropagation()}
              />
            ) : (
              <DropdownMenu>
                <DropdownMenuTrigger asChild>
                  <button type="button" className="dock-picker" aria-label="Choose playlist">
                    <span className="dock-picker-icon">
                      <Icon size={13} />
                    </span>
                    <span className="dock-picker-name">{current?.name ?? 'No playlist'}</span>
                    <ChevronDown size={13} />
                  </button>
                </DropdownMenuTrigger>
                <DropdownMenuContent align="start" className="w-56">
                  <DropdownMenuLabel>Playlist</DropdownMenuLabel>
                  <DropdownMenuSeparator />
                  {containers.map((p) => {
                    const PIcon = PLAYLIST_ICONS[p.icon] ?? ListMusic
                    return (
                      <DropdownMenuItem
                        key={p.id}
                        onSelect={() => setLayoutState({ panelPlaylistId: p.id })}
                        className={p.id === current?.id ? 'dock-item-active' : ''}
                      >
                        <PIcon size={13} />
                        <span>{p.name}</span>
                        {p.members.length > 0 ? (
                          <span className="dock-item-count">{p.members.length}</span>
                        ) : null}
                      </DropdownMenuItem>
                    )
                  })}
                  <DropdownMenuSeparator />
                  <DropdownMenuItem onSelect={addPlaylist}>
                    <Plus size={13} />
                    <span>New playlist</span>
                  </DropdownMenuItem>
                  <DropdownMenuItem onSelect={() => setRenaming(true)} disabled={!current}>
                    <Pencil size={13} />
                    <span>Rename</span>
                  </DropdownMenuItem>
                  {current ? (
                    <DropdownMenuSub>
                      <DropdownMenuSubTrigger>
                        <ListMusic size={13} />
                        <span>Icon</span>
                      </DropdownMenuSubTrigger>
                      <DropdownMenuSubContent>
                        {PLAYLIST_ICON_CHOICES.map((key) => {
                          const I = PLAYLIST_ICONS[key]
                          return (
                            <DropdownMenuItem key={key} onSelect={() => setPlaylistIcon(current.id, key)}>
                              <I size={13} />
                              <span>{key}</span>
                            </DropdownMenuItem>
                          )
                        })}
                      </DropdownMenuSubContent>
                    </DropdownMenuSub>
                  ) : null}
                  <DropdownMenuItem variant="destructive" onSelect={removeCurrent} disabled={!current}>
                    <Trash2 size={13} />
                    <span>Remove playlist</span>
                  </DropdownMenuItem>
                </DropdownMenuContent>
              </DropdownMenu>
            )}

            <button
              type="button"
              className="dock-config"
              aria-label="Playlist config"
              onClick={() => setConfigOpen(true)}
              disabled={!current}
            >
              config
            </button>

            <button
              type="button"
              className={`dock-config${isApplied ? ' dock-config-applied' : ''}`}
              aria-label={isApplied ? 'Playlist applied' : 'Apply playlist'}
              title={isApplied ? 'This playlist is running on the desktop' : 'Apply playlist'}
              onClick={() => {
                applyCurrent()
              }}
              disabled={!current || current.members.length === 0 || isApplied}
            >
              {isApplied ? 'applied' : 'apply'}
            </button>

            {isApplied ? (
              <button
                type="button"
                className="dock-config dock-config-stop"
                aria-label="Stop playlist and keep the last frame"
                title="Stop the rotation and leave the last frame on the desktop"
                onClick={() => {
                  playlistEngine.stopRotation()
                }}
              >
                stop
              </button>
            ) : null}

            <div className="dock-controls">
              <button
                type="button"
                className="dock-action"
                aria-label={minimized ? 'Restore panel' : 'Minimize panel'}
                title={minimized ? 'Restore panel' : 'Minimize panel'}
                onClick={onMinimize}
              >
                <Minus size={14} />
              </button>
              <button
                type="button"
                className="dock-action"
                aria-label={maximized ? 'Restore panel size' : 'Maximize panel'}
                title={maximized ? 'Restore panel size' : 'Maximize panel'}
                onClick={onMaximize}
              >
                <Square size={12} />
              </button>
              <button
                type="button"
                className="dock-action"
                aria-label="Close panel"
                title="Close panel"
                onClick={onClose}
              >
                <X size={14} />
              </button>
            </div>
          </div>

          {!minimized ? <PlaylistStrip playlistId={current?.id ?? null} /> : null}
        </>
      ) : null}

      {configOpen && current
        ? createPortal(
            <PlaylistEngineDialog initialPlaylistId={current.id} onClose={() => setConfigOpen(false)} />,
            document.body,
          )
        : null}
    </div>
  )
}
