import { AudioLines } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { OUTPUT_DEVICES, VISUALIZER_MODES } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function AudioSection({ wallpaper }) {
  const { audio } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['audio', key], value)

  return (
    <PropertySection title="Audio" icon={<AudioLines size={15} />}>
      <PropertyToggle label="Enable Audio" checked={audio.enabled} onChange={set('enabled')} />
      <PropertyToggle label="Mute" checked={audio.mute} onChange={set('mute')} />
      <PropertySlider label="Volume" value={audio.volume} onChange={set('volume')} min={0} max={100} suffix="%" />
      <PropertyDropdown
        label="Output Device"
        value={audio.outputDevice}
        options={OUTPUT_DEVICES}
        onChange={set('outputDevice')}
        description="Desktop runtime only — browsers cannot select an audio output device."
      />
      <PropertyToggle label="Audio Reactive" checked={audio.audioReactive} onChange={set('audioReactive')} />
      <PropertySlider
        label="Bass Sensitivity"
        value={audio.bassSensitivity}
        onChange={set('bassSensitivity')}
        min={0}
        max={100}
        suffix="%"
      />
      <PropertySlider
        label="Treble Sensitivity"
        value={audio.trebleSensitivity}
        onChange={set('trebleSensitivity')}
        min={0}
        max={100}
        suffix="%"
      />
      <PropertyDropdown
        label="Visualizer Mode"
        value={audio.visualizerMode}
        options={VISUALIZER_MODES}
        onChange={set('visualizerMode')}
      />
    </PropertySection>
  )
}
