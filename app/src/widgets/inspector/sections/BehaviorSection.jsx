import { Settings2 } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { updateWidgetProperty } from '../../store'

const BEHAVIOR_OPTIONS = [
  { key: 'visible', title: 'Visible', description: 'Show the widget on the desktop.', tooltip: 'Hidden widgets are not rendered.' },
  { key: 'enabled', title: 'Enabled', description: 'Allow the widget to run and react.', tooltip: 'Disabled widgets keep their layout but stop updating.' },
  { key: 'clickThrough', title: 'Click Through', description: 'Pass mouse events through the widget.', tooltip: 'Useful for overlays that should not block the desktop.' },
  { key: 'draggable', title: 'Draggable', description: 'Allow moving the widget with the mouse.', tooltip: 'Drag anywhere on the widget to reposition it.' },
  { key: 'lockPosition', title: 'Lock Position', description: 'Prevent the widget from being moved.', tooltip: 'Overrides Draggable.' },
  { key: 'keepOnScreen', title: 'Keep On Screen', description: 'Keep the widget inside monitor bounds.', tooltip: 'Auto-corrects position when moving or resizing.' },
  { key: 'savePosition', title: 'Save Position', description: 'Persist position across sessions.', tooltip: 'Restores the widget where you left it.' },
  { key: 'snapToGrid', title: 'Snap To Grid', description: 'Align to the 8px layout grid.', tooltip: 'Snaps x and y to multiples of 8.' },
  { key: 'snapToEdges', title: 'Snap To Edges', description: 'Snap to screen edges and other widgets.', tooltip: 'Magnet behavior while moving.' },
  { key: 'favorite', title: 'Favorite', description: 'Show in the Favorites section.', tooltip: 'Quick access to frequently used widgets.' },
  { key: 'ignoreMouse', title: 'Ignore Mouse', description: 'Ignore all pointer interaction.', tooltip: 'Combines Click Through with disabled hover states.' },
  { key: 'alwaysActive', title: 'Always Active', description: 'Keep updating when idle.', tooltip: 'Widgets update even when not focused.' },
]

export default function BehaviorSection({ widget }) {
  const { behavior } = widget
  const set = (key) => (value) => updateWidgetProperty(widget.id, ['behavior', key], value)
  return (
    <PropertySection title="Behavior" icon={<Settings2 size={15} />}>
      {BEHAVIOR_OPTIONS.map((option) => (
        <PropertyToggle
          key={option.key}
          label={option.title}
          description={option.description}
          tooltip={option.tooltip}
          checked={behavior[option.key]}
          onChange={set(option.key)}
        />
      ))}
    </PropertySection>
  )
}
