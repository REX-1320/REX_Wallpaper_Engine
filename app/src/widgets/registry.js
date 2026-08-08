import { Clock, CloudSun, StickyNote, CalendarDays, Music, Blocks } from 'lucide-react'

export const POSITION_MODES = [
  'Normal',
  'Top',
  'Bottom',
  'Desktop',
  'Behind Desktop',
  'Always On Top',
  'Always On Bottom',
  'Relative',
]

export const TRANSPARENCY_MODES = ['Opaque', 'Transparent', 'Glass', 'Acrylic', 'Mica']

export const THEME_OPTIONS = ['Default', 'Glass', 'Liquid Glass', 'Dark', 'Light', 'Custom']

export const LAYER_OPTIONS = ['Background', 'Desktop', 'Widget', 'Overlay', 'Floating', 'Topmost']

export const HOVER_ACTIONS = [
  'Do Nothing',
  'Show',
  'Hide',
  'Fade',
  'Expand',
  'Collapse',
  'Highlight',
  'Tooltip',
  'Run Command',
]

export const CLICK_ACTIONS = ['Open Settings', 'Run Command', 'Open URL', 'Launch App', 'Nothing']

export function detectMonitors() {
  const w = typeof screen !== 'undefined' ? screen.width : 0
  const h = typeof screen !== 'undefined' ? screen.height : 0
  return [{ id: 'display-1', label: `Display 1 · ${w}×${h} (Primary)` }]
}

const DEFAULT_BEHAVIOR = {
  visible: true,
  enabled: true,
  clickThrough: false,
  draggable: true,
  lockPosition: false,
  keepOnScreen: true,
  savePosition: true,
  snapToGrid: false,
  snapToEdges: true,
  favorite: false,
  ignoreMouse: false,
  alwaysActive: false,
}

const DEFAULT_APPEARANCE = {
  opacity: 90,
  transparencyMode: 'Glass',
  blur: 16,
  scale: 100,
  rotation: 0,
  theme: 'Glass',
}

const DEFAULT_LAYER = { loadOrder: 0, zIndex: 10, layer: 'Widget' }

const DEFAULT_INTERACTION = { onHover: 'Highlight', onClick: 'Open Settings' }

export function createWidgetFromTemplate(templateId, index = 0) {
  const def = WIDGET_TEMPLATES.find((t) => t.id === templateId)
  if (!def) return null
  return {
    id: def.id,
    templateId: def.id,
    name: def.name,
    category: def.category,
    position: {
      x: 32 + (index % 3) * 28,
      y: 24 + index * 36,
      width: def.width,
      height: def.height,
      monitor: 'display-1',
      positionMode: 'Normal',
    },
    behavior: { ...DEFAULT_BEHAVIOR },
    appearance: { ...DEFAULT_APPEARANCE },
    layer: { ...DEFAULT_LAYER },
    interaction: { ...DEFAULT_INTERACTION },
    info: {
      name: def.name,
      author: 'REX',
      version: '1.0.0',
      license: 'MIT',
      package: `rex.widgets.${def.id}`,
      category: def.category,
      tags: def.tags,
      description: def.description,
      lastUpdated: '2026-08-05',
      fileLocation: `C:\\REX\\widgets\\${def.id}\\${def.id}.rexw`,
      widgetId: def.id,
    },
  }
}

export const WIDGET_TEMPLATES = [
  { id: 'clock', name: 'Clock', icon: Clock, category: 'Clocks', width: 200, height: 72, tags: 'time,clock', description: 'Analog clock widget with live time.' },
  { id: 'weather', name: 'Weather', icon: CloudSun, category: 'Information', width: 220, height: 96, tags: 'weather,temperature', description: 'Current conditions and forecast.' },
  { id: 'notes', name: 'Notes', icon: StickyNote, category: 'Productivity', width: 240, height: 160, tags: 'notes,sticky', description: 'Quick sticky notes on the desktop.' },
  { id: 'calendar', name: 'Calendar', icon: CalendarDays, category: 'Productivity', width: 260, height: 220, tags: 'calendar,date', description: 'Month view calendar widget.' },
  { id: 'music', name: 'Music', icon: Music, category: 'Media', width: 240, height: 84, tags: 'music,player', description: 'Now playing media controls.' },
]

export const DEFAULT_ICON = Blocks

export function getTemplate(templateId) {
  return WIDGET_TEMPLATES.find((t) => t.id === templateId) ?? null
}
