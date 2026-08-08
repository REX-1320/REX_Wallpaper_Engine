import { motion } from 'motion/react'
import { Button } from '@/components/ui/button'
import { getTemplate } from './registry'
import { selectWidget, useWidgetStore } from './store'

const FAVORITE_IDS = ['clock', 'weather', 'notes']
const WIDGET_IDS = ['calendar', 'weather', 'notes', 'music']

function WidgetItem({ id, selected, onSelect }) {
  const template = getTemplate(id)
  const Icon = template?.icon
  return (
    <motion.div whileTap={{ scale: 0.97 }}>
      <Button
        variant="ghost"
        size="sm"
        className={`w-full justify-start gap-2 text-xs${selected ? ' bg-accent text-accent-foreground' : ''}`}
        onClick={onSelect}
        title={template?.description ?? ''}
      >
        {Icon ? <Icon className="size-3.5 opacity-70" /> : null}
        <span className="truncate">{template?.name ?? id}</span>
      </Button>
    </motion.div>
  )
}

function WidgetMenu({ title, ids }) {
  const { selectedId } = useWidgetStore()
  return (
    <div className="widgets-collection-section">
      <div className="widgets-panel-label">{title}</div>
      <div className="widgets-menu">
        {ids.map((id) => (
          <WidgetItem key={id} id={id} selected={id === selectedId} onSelect={() => selectWidget(id)} />
        ))}
      </div>
    </div>
  )
}

export default function WidgetsCollection() {
  return (
    <div className="widgets-collection">
      <WidgetMenu title="Favorites" ids={FAVORITE_IDS} />
      <WidgetMenu title="Widgets" ids={WIDGET_IDS} />
    </div>
  )
}
