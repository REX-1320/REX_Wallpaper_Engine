import { Move } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyNumber from '@/components/properties/PropertyNumber'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { POSITION_MODES, detectMonitors } from '../../registry'
import { updateWidgetProperty } from '../../store'

const MONITORS = detectMonitors()

export default function PositionSection({ widget }) {
  const { position } = widget
  const set = (key) => (value) => updateWidgetProperty(widget.id, ['position', key], value)
  return (
    <PropertySection title="Position" icon={<Move size={15} />}>
      <PropertyNumber label="X" tooltip="Horizontal position on the desktop." value={position.x} onChange={set('x')} step={8} />
      <PropertyNumber label="Y" tooltip="Vertical position on the desktop." value={position.y} onChange={set('y')} step={8} />
      <PropertyGroup title="Size">
        <PropertyNumber label="Width" value={position.width} onChange={set('width')} min={32} step={8} />
        <PropertyNumber label="Height" value={position.height} onChange={set('height')} min={24} step={8} />
      </PropertyGroup>
      <PropertyDropdown
        label="Display Monitor"
        tooltip="Monitor this widget is placed on. Detected automatically."
        value={position.monitor}
        options={MONITORS}
        onChange={set('monitor')}
      />
      <PropertyDropdown
        label="Position Mode"
        tooltip="How the widget attaches to the desktop."
        value={position.positionMode}
        options={POSITION_MODES}
        onChange={set('positionMode')}
      />
    </PropertySection>
  )
}
