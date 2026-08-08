import { ListMusic } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { PLAYLIST_ORDERS, TRANSITIONS } from '../../registry'
import { updateWallpaperProperty, useWallpaperStore } from '../../store'

export default function PlaylistSection({ wallpaper }) {
  const { playlists } = useWallpaperStore()
  const { playlist } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['playlist', key], value)

  return (
    <PropertySection title="Playlist" icon={<ListMusic size={15} />}>
      <PropertyDropdown
        label="Current Playlist"
        value={playlist.currentPlaylist}
        options={playlists.map((p) => p.name)}
        onChange={set('currentPlaylist')}
      />
      <PropertyDropdown
        label="Wallpaper Order"
        value={playlist.order}
        options={PLAYLIST_ORDERS}
        onChange={set('order')}
      />
      <PropertyToggle label="Shuffle" checked={playlist.shuffle} onChange={set('shuffle')} />
      <PropertyToggle label="Repeat" checked={playlist.repeat} onChange={set('repeat')} />
      <PropertyDropdown
        label="Transition"
        value={playlist.transition}
        options={TRANSITIONS}
        onChange={set('transition')}
      />
      <PropertySlider
        label="Transition Duration"
        value={playlist.transitionDuration}
        onChange={set('transitionDuration')}
        min={0}
        max={10}
        step={0.5}
        suffix="s"
      />
      <PropertyToggle label="Auto Change" checked={playlist.autoChange} onChange={set('autoChange')} />
      <PropertySlider
        label="Change Every"
        value={playlist.changeEvery}
        onChange={set('changeEvery')}
        min={1}
        max={120}
        suffix="m"
      />
      <PropertyToggle label="Idle Trigger" checked={playlist.idleTrigger} onChange={set('idleTrigger')} />
      <PropertyToggle label="Battery Trigger" checked={playlist.batteryTrigger} onChange={set('batteryTrigger')} />
      <PropertyToggle label="Weather Trigger" checked={playlist.weatherTrigger} onChange={set('weatherTrigger')} />
    </PropertySection>
  )
}
