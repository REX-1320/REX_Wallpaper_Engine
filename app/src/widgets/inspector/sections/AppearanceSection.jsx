import { Palette } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { TRANSPARENCY_MODES, THEME_OPTIONS } from '../../registry'
import { updateWidgetProperty } from '../../store'

export default function AppearanceSection({ widget }) {
  const { appearance } = widget
  const set = (key) => (value) => updateWidgetProperty(widget.id, ['appearance', key], value)
  return (
    <PropertySection title="Appearance" icon={<Palette size={15} />}>
      <PropertySlider label="Opacity" tooltip="Overall opacity of the widget." value={appearance.opacity} onChange={set('opacity')} min={0} max={100} suffix="%" />
      <PropertyDropdown
        label="Transparency Mode"
        tooltip="How the widget background is composited."
        value={appearance.transparencyMode}
        options={TRANSPARENCY_MODES}
        onChange={set('transparencyMode')}
      />
      <PropertySlider label="Blur" tooltip="Background blur radius." value={appearance.blur} onChange={set('blur')} min={0} max={64} step={1} suffix="px" />
      <PropertySlider label="Scale" tooltip="Scale relative to the original size." value={appearance.scale} onChange={set('scale')} min={10} max={300} step={5} suffix="%" />
      <PropertySlider label="Rotation" tooltip="Rotation in degrees." value={appearance.rotation} onChange={set('rotation')} min={0} max={360} step={1} suffix="Â°" />
      <PropertyDropdown
        label="Theme"
        tooltip="Visual theme applied to the widget."
        value={appearance.theme}
        options={THEME_OPTIONS}
        onChange={set('theme')}
      />
    </PropertySection>
  )
}
