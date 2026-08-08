import { motion, useMotionValue } from 'motion/react'
import { getTemplate, DEFAULT_ICON } from './registry'
import { updateWidgetProperty } from './store'

const THEME_TINTS = {
  Default: 'rgba(30, 30, 30, 0.55)',
  Glass: 'rgba(30, 30, 30, 0.4)',
  'Liquid Glass': 'rgba(255, 255, 255, 0.14)',
  Dark: 'rgba(10, 10, 10, 0.78)',
  Light: 'rgba(255, 255, 255, 0.88)',
  Custom: 'rgba(70, 70, 120, 0.5)',
}

const GRID = 8
const EDGE_SNAP = 12

export default function WidgetVisual({ widget, selected, onSelect, bounds = { width: 0, height: 0 } }) {
  const { position, appearance, layer, behavior } = widget
  const template = getTemplate(widget.templateId)
  const Icon = template?.icon ?? DEFAULT_ICON
  const glassy = appearance.transparencyMode !== 'Opaque'
  const light = appearance.theme === 'Light' || appearance.theme === 'Liquid Glass'
  const draggable = behavior.draggable && !behavior.lockPosition
  const clickThrough = behavior.clickThrough || behavior.ignoreMouse

  const offsetX = useMotionValue(0)
  const offsetY = useMotionValue(0)

  const handleDragEnd = () => {
    let x = position.x + offsetX.get()
    let y = position.y + offsetY.get()

    if (behavior.snapToGrid) {
      x = Math.round(x / GRID) * GRID
      y = Math.round(y / GRID) * GRID
    }

    if (bounds.width > 0) {
      const right = Math.max(bounds.width - position.width, 0)
      const bottom = Math.max(bounds.height - position.height, 0)

      if (behavior.snapToEdges) {
        if (Math.abs(x) < EDGE_SNAP) x = 0
        if (Math.abs(y) < EDGE_SNAP) y = 0
        if (Math.abs(x - right) < EDGE_SNAP) x = right
        if (Math.abs(y - bottom) < EDGE_SNAP) y = bottom
      }

      if (behavior.keepOnScreen) {
        x = Math.min(Math.max(x, 0), right)
        y = Math.min(Math.max(y, 0), bottom)
      }
    }

    offsetX.set(0)
    offsetY.set(0)
    updateWidgetProperty(widget.id, ['position', 'x'], Math.round(x))
    updateWidgetProperty(widget.id, ['position', 'y'], Math.round(y))
  }

  const style = {
    left: position.x,
    top: position.y,
    width: position.width,
    height: position.height,
    zIndex: layer.zIndex,
    x: offsetX,
    y: offsetY,
    pointerEvents: clickThrough ? 'none' : undefined,
    background: THEME_TINTS[appearance.theme] ?? THEME_TINTS.Default,
    backdropFilter: glassy ? `blur(${appearance.blur}px) saturate(150%)` : undefined,
    WebkitBackdropFilter: glassy ? `blur(${appearance.blur}px) saturate(150%)` : undefined,
    display: behavior.visible ? undefined : 'none',
    color: light ? '#1a1a1a' : '#f5f5f5',
  }

  return (
    <motion.button
      type="button"
      className={`widget-visual${draggable ? ' draggable' : ''}`}
      style={style}
      initial={{ opacity: 0, scale: 0.8 }}
      animate={{
        opacity: appearance.opacity / 100,
        scale: appearance.scale / 100,
        rotate: appearance.rotation,
        borderColor: selected ? 'rgba(255, 255, 255, 0.9)' : 'rgba(255, 255, 255, 0.2)',
        boxShadow: selected
          ? '0 0 0 2px rgba(255, 255, 255, 0.85), 0 10px 28px rgba(0, 0, 0, 0.35)'
          : '0 0 0 0px rgba(255, 255, 255, 0)',
      }}
      exit={{ opacity: 0, scale: 0.8 }}
      transition={{ type: 'spring', stiffness: 320, damping: 28 }}
      whileHover={{ scale: selected ? undefined : (appearance.scale / 100) * 1.04, cursor: 'grab' }}
      whileTap={{ scale: (appearance.scale / 100) * 0.97 }}
      whileDrag={{ zIndex: 1000, cursor: 'grabbing' }}
      drag={draggable && !clickThrough}
      dragMomentum={false}
      dragElastic={0}
      onDragStart={() => onSelect(null)}
      onDragEnd={handleDragEnd}
      onClick={onSelect}
      title={`${widget.name} — drag to move, click to select`}
    >
      <Icon size={20} />
      <span className="widget-visual-name">{widget.name}</span>
      <span className="widget-visual-cat">{widget.category}</span>
    </motion.button>
  )
}
