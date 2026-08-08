import { Palette } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertyColor from '@/components/properties/PropertyColor'
import { updateWallpaperProperty } from '../../store'

export default function AppearanceSection({ wallpaper }) {
  const { appearance } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['appearance', key], value)

  return (
    <PropertySection title="Appearance" icon={<Palette size={15} />}>
      <PropertySlider label="Opacity" value={appearance.opacity} onChange={set('opacity')} min={0} max={100} suffix="%" />
      <PropertyGroup title="Color Adjustments">
        <PropertySlider label="Brightness" value={appearance.brightness} onChange={set('brightness')} min={0} max={200} suffix="%" />
        <PropertySlider label="Contrast" value={appearance.contrast} onChange={set('contrast')} min={0} max={200} suffix="%" />
        <PropertySlider label="Gamma" value={appearance.gamma} onChange={set('gamma')} min={0.1} max={3} step={0.05} suffix="×" />
        <PropertySlider label="Saturation" value={appearance.saturation} onChange={set('saturation')} min={0} max={200} suffix="%" />
        <PropertySlider label="Hue" value={appearance.hue} onChange={set('hue')} min={0} max={360} suffix="°" />
        <PropertySlider label="Temperature" value={appearance.temperature} onChange={set('temperature')} min={0} max={100} suffix="%" />
        <PropertySlider label="Vibrance" value={appearance.vibrance} onChange={set('vibrance')} min={0} max={200} suffix="%" />
        <PropertySlider label="Exposure" value={appearance.exposure} onChange={set('exposure')} min={0} max={200} suffix="%" />
      </PropertyGroup>
      <PropertyGroup title="Effects">
        <PropertySlider label="Sharpen" value={appearance.sharpen} onChange={set('sharpen')} min={0} max={100} suffix="%" />
        <PropertySlider label="Blur" value={appearance.blur} onChange={set('blur')} min={0} max={50} suffix="px" />
        <PropertySlider label="Vignette" value={appearance.vignette} onChange={set('vignette')} min={0} max={100} suffix="%" />
        <PropertySlider label="Noise" value={appearance.noise} onChange={set('noise')} min={0} max={100} suffix="%" />
        <PropertyToggle label="Grayscale" checked={appearance.grayscale > 0} onChange={(v) => set('grayscale')(v ? 100 : 0)} />
        <PropertyToggle label="Invert" checked={appearance.invert > 0} onChange={(v) => set('invert')(v ? 100 : 0)} />
      </PropertyGroup>
      <PropertyGroup title="Overlays">
        <PropertyColor label="Tint" value={appearance.tint} onChange={set('tint')} />
        <PropertyColor label="Color Overlay" value={appearance.colorOverlay} onChange={set('colorOverlay')} />
      </PropertyGroup>
    </PropertySection>
  )
}
