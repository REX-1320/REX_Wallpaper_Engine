import { Play, Pause, Square, RotateCcw } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyTimeline from '@/components/properties/PropertyTimeline'
import PropertyButton from '@/components/properties/PropertyButton'
import { parseDuration } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function PlaybackSection({ wallpaper }) {
  const { playback } = wallpaper
  const duration = parseDuration(wallpaper.info.duration)
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['playback', key], value)

  return (
    <PropertySection title="Playback" icon={<Play size={15} />}>
      <PropertyGroup title="Transport">
        <div className="grid grid-cols-2 gap-2">
          <PropertyButton onClick={() => set('playing')(true)}>
            <Play size={14} /> Play
          </PropertyButton>
          <PropertyButton onClick={() => set('paused')(true)}>
            <Pause size={14} /> Pause
          </PropertyButton>
          <PropertyButton onClick={() => set('playing')(false)}>
            <Square size={14} /> Stop
          </PropertyButton>
          <PropertyButton onClick={() => set('startPosition')(0)}>
            <RotateCcw size={14} /> Restart
          </PropertyButton>
        </div>
      </PropertyGroup>
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
