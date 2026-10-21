import { SunMedium } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertySlider from '@/components/properties/PropertySlider'
import { updateWallpaperProperty } from '../../store'

// Colour adjustments the engine applies live, per frame: the renderer packs
// brightness / contrast / saturation into its b0 root constants every frame
// (`D3D12Renderer.cpp:2590-2592`). The engine's scale is 0..2 with 1.0 =
// unchanged; the app keeps percent 0..200 where 100 = unchanged.
//
// Video-only: the static (image) adapter has no colour pipeline, so the
// capability list decides whether this group exists at all.
const SLIDERS = [
  { key: 'brightness', label: 'Brightness' },
  { key: 'contrast', label: 'Contrast' },
  { key: 'saturation', label: 'Saturation' },
]

export default function PictureSection({ wallpaper, applied = false, capabilities = [] }) {
  const supported = new Set(capabilities)
  if (!SLIDERS.some((s) => supported.has(s.key))) return null

  const appearance = wallpaper.appearance ?? {}
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['appearance', key], value)

  return (
    <PropertySection title="Picture" icon={<SunMedium size={15} />}>
      {SLIDERS.filter((s) => supported.has(s.key)).map(({ key, label }) => (
        <PropertySlider
          key={key}
          label={label}
          value={appearance[key] ?? 100}
          onChange={set(key)}
          min={0}
          max={200}
          step={5}
          suffix="%"
        />
      ))}
      <p className="insp-hint">100% is unchanged.</p>
      {!applied ? (
        <p className="insp-hint">Saved for this wallpaper; it applies when you put it on the desktop.</p>
      ) : null}
    </PropertySection>
  )
}
