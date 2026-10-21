import { Play } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { updateWallpaperProperty } from '../../store'

const AUDIO_OPTIONS = [
  { value: 'auto', label: 'Auto' },
  { value: 'mono', label: 'Mono' },
  { value: 'stereo', label: 'Stereo' },
  { value: 'quad', label: 'Quad' },
  { value: 'surround51', label: '5.1' },
]

// Video-only, engine-backed controls. `capabilities` comes from
// DESKTOP_CAPABILITIES[type] (the same contract the runtime advertises), so a
// control is never rendered when the engine cannot apply it.
//
// Audio controls are hidden for files without an audio track: mute/volume on a
// silent clip do nothing, and offering them is a lie about the file.
export default function PlaybackSection({ wallpaper, applied, capabilities = [], media }) {
  if (wallpaper.type !== 'VIDEO') return null

  const { playback } = wallpaper
  const supported = new Set(capabilities)
  const hasAudioTrack = media?.hasAudio !== false
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['playback', key], value)

  return (
    <PropertySection title="Playback" icon={<Play size={15} />}>
      {hasAudioTrack && supported.has('mute') ? (
        <PropertyToggle label="Mute" checked={playback.mute} onChange={set('mute')} />
      ) : null}
      {hasAudioTrack && supported.has('volume') ? (
        <PropertySlider
          label="Volume"
          value={playback.volume}
          onChange={set('volume')}
          min={0}
          max={100}
          suffix="%"
        />
      ) : null}
      {supported.has('speed') ? (
        <PropertySlider
          label="Speed"
          value={playback.speed}
          onChange={set('speed')}
          min={0.25}
          max={2}
          step={0.05}
          suffix="×"
        />
      ) : null}
      {supported.has('loop') ? (
        <PropertyToggle label="Loop" checked={playback.loop} onChange={set('loop')} />
      ) : null}
      {hasAudioTrack && supported.has('audioMode') ? (
        <PropertyDropdown
          label="Output"
          value={playback.audioMode ?? 'auto'}
          onChange={set('audioMode')}
          options={AUDIO_OPTIONS}
        />
      ) : null}
      {!hasAudioTrack ? (
        <p className="insp-hint">This file has no audio track.</p>
      ) : null}
      {!applied ? (
        <p className="insp-hint">Saved for this wallpaper; it applies when you put it on the desktop.</p>
      ) : null}
    </PropertySection>
  )
}
