import { MonitorCog } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { updateWallpaperProperty } from '../../store'

const MONITOR_OPTIONS = [
  { key: 'differentWallpaper', title: 'Different Wallpaper' },
  { key: 'spanAcross', title: 'Span Across Displays' },
  { key: 'mirror', title: 'Mirror' },
  { key: 'independentPlayback', title: 'Independent Playback' },
  { key: 'independentAudio', title: 'Independent Audio' },
  { key: 'independentEffects', title: 'Independent Effects' },
]

export default function MonitorSection({ wallpaper }) {
  const { monitor } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['monitor', key], value)

  return (
    <PropertySection title="Multi Monitor" icon={<MonitorCog size={15} />}>
      <PropertyDropdown
        label="Apply To"
        value={monitor.applyTo}
        options={['Monitor 1', 'Monitor 2', 'All']}
        onChange={set('applyTo')}
      />
      {MONITOR_OPTIONS.map(({ key, title }) => (
        <PropertyToggle key={key} label={title} checked={monitor[key]} onChange={set(key)} />
      ))}
    </PropertySection>
  )
}
