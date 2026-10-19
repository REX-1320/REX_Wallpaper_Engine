import { Play } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyTimeline from '@/components/properties/PropertyTimeline'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { parseDuration } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function PlaybackSection({ wallpaper }) {
  const { playback } = wallpaper
  const duration = parseDuration(wallpaper.info.duration)
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['playback', key], value)

  return (
    <PropertySection title="Playback" icon={<Play size={15} />}>
      {/* Transport (Play/Pause/Stop/Restart) used to live here: the buttons
          only mutated store flags that no apply path reads (audit 5.10 #16).
          They return when the runtime exposes real transport commands. */}
      <PropertyToggle label="Loop" checked={playback.loop} onChange={set('loop')} />
      <PropertyToggle label="Mute" checked={playback.mute} onChange={set('mute')} />
      <PropertyToggle label="Autoplay" checked={playback.autoplay} onChange={set('autoplay')} />
      <PropertyToggle label="Random Start" checked={playback.randomStart} onChange={set('randomStart')} />
      <PropertyToggle
        label="Resume Previous Position"
        checked={playback.resumePrevious}
        onChange={set('resumePrevious')}
      />
      <PropertySlider label="Volume" value={playback.volume} onChange={set('volume')} min={0} max={100} suffix="%" />
      <PropertyDropdown
        label="Output"
        value={playback.audioMode ?? 'auto'}
        onChange={set('audioMode')}
        options={[
          { value: 'auto', label: 'Auto' },
          { value: 'mono', label: 'Mono' },
          { value: 'stereo', label: 'Stereo' },
          { value: 'quad', label: 'Quad' },
          { value: 'surround51', label: '5.1' },
        ]}
      />
      <PropertySlider
        label="Playback Speed"
        value={playback.speed}
        onChange={set('speed')}
        min={0.25}
        max={2}
        step={0.05}
        suffix="×"
      />
      <PropertyTimeline
        label="Start Position"
        value={playback.startPosition}
        onChange={set('startPosition')}
        min={0}
        max={duration}
      />
      <PropertyToggle label="Seek Timeline" checked={playback.seekTimeline} onChange={set('seekTimeline')} />
    </PropertySection>
  )
}
