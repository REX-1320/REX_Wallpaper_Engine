import { Wrench } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyText from '@/components/properties/PropertyText'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { ENV_OPTIONS } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function AdvancedSection({ wallpaper }) {
  const { advanced } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['advanced', key], value)

  return (
    <PropertySection title="Advanced" icon={<Wrench size={15} />}>
      <PropertyGroup title="Configuration">
        <PropertyText label="Metadata" value={advanced.metadata} mono readOnly />
        <PropertyText label="JSON" value='{"key":"value"}' mono readOnly />
        <PropertyText label="Variables" value={advanced.variables} mono readOnly />
        <PropertyText label="Arguments" value={advanced.arguments} mono readOnly />
      </PropertyGroup>
      <PropertyDropdown
        label="Environment Variables"
        value={ENV_OPTIONS[0]}
        options={ENV_OPTIONS}
        onChange={set('environmentVariables')}
      />
      <PropertyToggle label="Developer Mode" checked={advanced.developerMode} onChange={set('developerMode')} />
      <PropertyToggle label="Logs" checked={advanced.logs} onChange={set('logs')} />
      <PropertyToggle label="Diagnostics" checked={advanced.diagnostics} onChange={set('diagnostics')} />
      <PropertyToggle label="Renderer Statistics" checked={advanced.rendererStats} onChange={set('rendererStats')} />
    </PropertySection>
  )
}
