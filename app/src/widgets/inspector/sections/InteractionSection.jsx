import { MousePointerClick } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import { HOVER_ACTIONS, CLICK_ACTIONS } from '../../registry'
import { updateWidgetProperty } from '../../store'

export default function InteractionSection({ widget }) {
  const { interaction } = widget
  const set = (key) => (value) => updateWidgetProperty(widget.id, ['interaction', key], value)
  return (
    <PropertySection title="Interaction" icon={<MousePointerClick size={15} />}>
      <PropertyDropdown label="On Hover" tooltip="Action when the pointer enters the widget." value={interaction.onHover} options={HOVER_ACTIONS} onChange={set('onHover')} />
      <PropertyDropdown label="On Click" tooltip="Action when the widget is clicked." value={interaction.onClick} options={CLICK_ACTIONS} onChange={set('onClick')} />
    </PropertySection>
  )
}
