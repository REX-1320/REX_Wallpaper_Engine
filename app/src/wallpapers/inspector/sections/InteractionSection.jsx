import { MousePointerClick } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { updateWallpaperProperty } from '../../store'

const INTERACTION_OPTIONS = [
  { key: 'mouseInteraction', title: 'Mouse Interaction' },
  { key: 'clickThrough', title: 'Click Through' },
  { key: 'followCursor', title: 'Follow Cursor' },
  { key: 'cursorGlow', title: 'Cursor Glow' },
  { key: 'cursorRipple', title: 'Cursor Ripple' },
  { key: 'particles', title: 'Particles' },
  { key: 'touchSupport', title: 'Touch Support' },
  { key: 'gestureSupport', title: 'Gesture Support' },
]

export default function InteractionSection({ wallpaper }) {
  const { interaction } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['interaction', key], value)

  return (
    <PropertySection title="Interaction" icon={<MousePointerClick size={15} />}>
      {INTERACTION_OPTIONS.map(({ key, title }) => (
        <PropertyToggle key={key} label={title} checked={interaction[key]} onChange={set(key)} />
      ))}
    </PropertySection>
  )
}
