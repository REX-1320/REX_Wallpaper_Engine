import { Monitor } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertyMonitorPicker from '@/components/properties/PropertyMonitorPicker'
import PropertyNumber from '@/components/properties/PropertyNumber'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertyColor from '@/components/properties/PropertyColor'
import { ALIGNMENTS, WALLPAPER_MODES } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function DisplaySection({ wallpaper, expandable }) {
  const { display } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['display', key], value)

  const controls = (
    <>
      <PropertyMonitorPicker label="Monitor" value={display.monitor} onChange={set('monitor')} />
      <PropertyDropdown
        label="Wallpaper Mode"
        value={display.mode}
        options={WALLPAPER_MODES}
        onChange={set('mode')}
      />
      <PropertyDropdown
        label="Alignment"
        value={display.alignment}
        options={ALIGNMENTS}
        onChange={set('alignment')}
      />
      <PropertyGroup title="Transform">
        <PropertyNumber
          label="Offset X"
          value={display.offsetX}
          onChange={set('offsetX')}
          min={-2000}
          max={2000}
          step={8}
          suffix="px"
        />
        <PropertyNumber
          label="Offset Y"
          value={display.offsetY}
          onChange={set('offsetY')}
          min={-2000}
          max={2000}
          step={8}
          suffix="px"
        />
        <PropertySlider label="Scale" value={display.scale} onChange={set('scale')} min={50} max={200} suffix="%" />
        <PropertySlider
          label="Rotation"
          value={display.rotation}
          onChange={set('rotation')}
          min={0}
          max={360}
          suffix="°"
        />
        <PropertyToggle label="Flip Horizontal" checked={display.flipH} onChange={set('flipH')} />
        <PropertyToggle label="Flip Vertical" checked={display.flipV} onChange={set('flipV')} />
        <PropertySlider label="Crop" value={display.crop} onChange={set('crop')} min={0} max={100} suffix="%" />
      </PropertyGroup>
      <PropertyColor label="Background Color" value={display.backgroundColor} onChange={set('backgroundColor')} />
      <PropertyColor label="Letterbox Color" value={display.letterboxColor} onChange={set('letterboxColor')} />
    </>
  )

  if (!expandable) return controls

  return (
    <PropertySection title="Display" icon={<Monitor size={15} />}>
      {controls}
    </PropertySection>
  )
}
