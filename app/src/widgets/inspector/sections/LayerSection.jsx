import { Layers } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyNumber from '@/components/properties/PropertyNumber'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { LAYER_OPTIONS } from '../../registry'
import { updateWidgetProperty } from '../../store'

export default function LayerSection({ widget }) {
  const { layer } = widget
  const set = (key) => (value) => updateWidgetProperty(widget.id, ['layer', key], value)
  return (
    <PropertySection title="Layer" icon={<Layers size={15} />}>
      <PropertyNumber label="Load Order" tooltip="Order in which widgets load at startup." value={layer.loadOrder} onChange={set('loadOrder')} min={0} step={1} />
      <PropertyNumber label="Z Index" tooltip="Stacking order on the desktop." value={layer.zIndex} onChange={set('zIndex')} min={-1000} max={1000} step={1} />
      <PropertyDropdown label="Layer" tooltip="Which desktop layer the widget belongs to." value={layer.layer} options={LAYER_OPTIONS} onChange={set('layer')} />
    </PropertySection>
  )
}
