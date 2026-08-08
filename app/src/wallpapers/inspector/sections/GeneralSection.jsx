import { Settings2 } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { updateWallpaperProperty } from '../../store'

const GENERAL_OPTIONS = [
  { key: 'enabled', title: 'Enabled', description: 'Run this wallpaper.', tooltip: 'Disabled wallpapers are not applied.' },
  { key: 'favorite', title: 'Favorite', description: 'Pin to the favorites list.', tooltip: 'Quick access from the collection.' },
  { key: 'visible', title: 'Visible', description: 'Show on the desktop.', tooltip: 'Hidden wallpapers keep their settings.' },
  { key: 'startAutomatically', title: 'Start Automatically', description: 'Apply at session start.', tooltip: 'Starts when you sign in.' },
  { key: 'loadOnStartup', title: 'Load on Startup', description: 'Preload assets at boot.', tooltip: 'Reduces delay when applying.' },
  { key: 'pauseWhenHidden', title: 'Pause When Hidden', description: 'Freeze playback when not visible.', tooltip: 'Saves resources when another wallpaper is active.' },
  { key: 'autoResume', title: 'Auto Resume', description: 'Resume playback when visible again.', tooltip: 'Continues from the paused frame.' },
  { key: 'pinWallpaper', title: 'Pin Wallpaper', description: 'Lock to a specific display.', tooltip: 'Stays on its monitor across layouts.' },
  { key: 'lockEditing', title: 'Lock Editing', description: 'Prevent further property changes.', tooltip: 'Protects shared presets.' },
]

export default function GeneralSection({ wallpaper }) {
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['general', key], value)
  return (
    <PropertySection title="General" icon={<Settings2 size={15} />}>
      {GENERAL_OPTIONS.map(({ key, title, description, tooltip }) => (
        <PropertyToggle
          key={key}
          label={title}
          description={description}
          tooltip={tooltip}
          checked={wallpaper.general[key]}
          onChange={set(key)}
        />
      ))}
    </PropertySection>
  )
}
