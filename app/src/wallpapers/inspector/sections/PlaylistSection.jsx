import { ListPlus, Music } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyButton from '@/components/properties/PropertyButton'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import {
  addPlaylistMembers,
  isPlaylistMember,
  updateWallpaperProperty,
  useWallpaperStore,
} from '../../store'

// Playlist membership belongs here (it is a per-wallpaper decision) while the
// destructive actions stay in the gallery tile menu, which owns tiles and the
// persisted library entry. Delete/duplicate in the old inspector only mutated
// the store, so the tile and library entry survived — silent no-ops.
export default function PlaylistSection({ wallpaper }) {
  const { playlists } = useWallpaperStore()
  const filePath = typeof wallpaper?.filePath === 'string' ? wallpaper.filePath : ''
  const targets = playlists.filter((playlist) => !playlist.virtual && playlist.name !== 'None')

  const addTo = (playlist) => {
    if (!filePath) return
    addPlaylistMembers(playlist.id, [filePath])
    // Same rule as the gallery's "add to playlist": joining a playlist also
    // makes it the file's governing playlist for the engine.
    updateWallpaperProperty(wallpaper.id, ['playlist', 'currentPlaylist'], playlist.name)
  }

  return (
    <PropertySection title="Playlist" icon={<Music size={15} />} defaultOpen={false}>
      <DropdownMenu>
        <DropdownMenuTrigger asChild>
          <PropertyButton disabled={!filePath || targets.length === 0}>
            <ListPlus size={14} /> Add to playlist
          </PropertyButton>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="start" className="w-52">
          <DropdownMenuLabel className="text-[10px] uppercase tracking-[0.12em] text-muted-foreground">
            Playlists
          </DropdownMenuLabel>
          <DropdownMenuSeparator />
          {targets.map((playlist) => {
            const member = isPlaylistMember(playlist.id, filePath)
            return (
              <DropdownMenuItem
                key={playlist.id}
                disabled={!filePath || member}
                onSelect={() => addTo(playlist)}
              >
                <span className="truncate">{playlist.name}</span>
                {member ? (
                  <span className="ml-auto text-[10px] text-muted-foreground">Added</span>
                ) : null}
              </DropdownMenuItem>
            )
          })}
          {targets.length === 0 ? <DropdownMenuItem disabled>No playlists yet</DropdownMenuItem> : null}
        </DropdownMenuContent>
      </DropdownMenu>
      {!filePath ? (
        <p className="insp-hint">This wallpaper has no file on disk, so it cannot join a playlist.</p>
      ) : null}
    </PropertySection>
  )
}
