import { Monitor } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import { fitModesFor, normalizeDisplayMode } from '../../wallpaperSettings'
import { updateWallpaperProperty } from '../../store'

// The one display choice users actually need: how the file fills the screen.
// Windows exposes exactly this (fit / fill / span / tile / center) and it is
// the engine's only image property. Span/Fit Width/Fit Height collapse onto
// these modes (see ENGINE_MODE_BY_MODE), and Tile is dropped for videos
// because the engine refuses it.
const MODE_LABELS = {
  Cover: 'Fill',
  Contain: 'Fit',
  Stretch: 'Stretch',
  Center: 'Center',
  Tile: 'Tile',
}

export default function DisplaySection({ wallpaper, applied = false, inline = false }) {
  if (!wallpaper) return null
  const current = normalizeDisplayMode(wallpaper.display?.mode) ?? 'Cover'
  const modes = fitModesFor(wallpaper.type)
  const setMode = (mode) => updateWallpaperProperty(wallpaper.id, ['display', 'mode'], mode)

  const body = [
    <div key="modes" className="fit-modes" role="group" aria-label="Fit / fill mode">
      {modes.map((mode) => (
        <button
          key={mode}
          type="button"
          className={`fit-mode${current === mode ? ' active' : ''}`}
          aria-pressed={current === mode}
          title={`${MODE_LABELS[mode] ?? mode} the wallpaper on the desktop`}
          onClick={() => setMode(mode)}
        >
          {MODE_LABELS[mode] ?? mode}
        </button>
      ))}
    </div>,
    !applied ? (
      <p key="hint" className="insp-hint">
        Saved for this wallpaper; it applies when you put it on the desktop.
      </p>
    ) : null,
  ]

  if (inline) return <div className="fit-modes-inline">{body}</div>

  return (
    <PropertySection title="Display" icon={<Monitor size={15} />} expandable={false}>
      {body}
    </PropertySection>
  )
}
