import { Gauge } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertyText from '@/components/properties/PropertyText'
import { FRAME_RATE_LIMITS, POWER_MODES, RENDERER_OPTIONS, RENDER_RESOLUTIONS } from '../../registry'
import { updateWallpaperProperty } from '../../store'

const PAUSE_OPTIONS = [
  { key: 'pauseOnBattery', title: 'Pause On Battery' },
  { key: 'pauseFullscreen', title: 'Pause Fullscreen Apps' },
  { key: 'pauseGames', title: 'Pause Games' },
  { key: 'pauseRemoteDesktop', title: 'Pause Remote Desktop' },
  { key: 'pauseVideoCalls', title: 'Pause Video Calls' },
]

export default function PerformanceSection({ wallpaper }) {
  const { performance } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['performance', key], value)

  return (
    <PropertySection title="Performance" icon={<Gauge size={15} />}>
      <PropertyDropdown
        label="Renderer"
        value={performance.renderer}
        options={RENDERER_OPTIONS}
        onChange={set('renderer')}
      />
      <PropertyDropdown
        label="Frame Rate Limit"
        value={performance.frameRateLimit}
        options={FRAME_RATE_LIMITS}
        onChange={set('frameRateLimit')}
      />
      <PropertyDropdown
        label="Render Resolution"
        value={performance.renderResolution}
        options={RENDER_RESOLUTIONS}
        onChange={set('renderResolution')}
      />
      <PropertyDropdown
        label="Power Mode"
        value={performance.powerMode}
        options={POWER_MODES}
        onChange={set('powerMode')}
      />
      <PropertyGroup title="Auto Pause">
        {PAUSE_OPTIONS.map(({ key, title }) => (
          <PropertyToggle
            key={key}
            label={title}
            checked={performance[key]}
            onChange={set(key)}
          />
        ))}
      </PropertyGroup>
      <PropertyGroup title="Statistics">
        <PropertyText label="VRAM Usage" value="1.2 GB / 8.0 GB" readOnly />
        <PropertyText label="Estimated GPU Usage" value="18%" readOnly />
        <PropertyText label="Estimated CPU Usage" value="4%" readOnly />
      </PropertyGroup>
    </PropertySection>
  )
}
