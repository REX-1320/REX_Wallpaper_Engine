import { useSyncExternalStore } from 'react'
import { createWidgetFromTemplate, WIDGET_TEMPLATES } from './registry'

const widgets = new Map()
let selectedId = null
const listeners = new Set()
let snapshot = null
let seeded = false
let cloneCounter = 0

function bump() {
  snapshot = null
  for (const listener of listeners) listener()
}

export function seedWidgets() {
  if (seeded) return
  WIDGET_TEMPLATES.forEach((template, index) => {
    const widget = createWidgetFromTemplate(template.id, index)
    if (widget) widgets.set(widget.id, widget)
  })
  selectedId = 'clock'
  seeded = true
  bump()
}

export function subscribe(listener) {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

export function getSnapshot() {
  if (!snapshot) snapshot = { widgets: [...widgets.values()], selectedId }
  return snapshot
}

export function useWidgetStore() {
  return useSyncExternalStore(subscribe, getSnapshot)
}

export function selectWidget(id) {
  if (id !== null && !widgets.has(id)) return
  selectedId = id
  bump()
}

export function updateWidgetProperty(id, path, value) {
  const widget = widgets.get(id)
  if (!widget) return
  const clone = structuredClone(widget)
  let target = clone
  for (let i = 0; i < path.length - 1; i += 1) target = target[path[i]]
  target[path[path.length - 1]] = value
  widgets.set(id, clone)
  bump()
}

export function duplicateWidget(id) {
  const widget = widgets.get(id)
  if (!widget) return
  const clone = structuredClone(widget)
  clone.id = `${id}-${(cloneCounter += 1)}`
  clone.name = `${widget.name} Copy`
  clone.info = { ...clone.info, widgetId: clone.id }
  clone.position = { ...clone.position, x: clone.position.x + 24, y: clone.position.y + 24 }
  widgets.set(clone.id, clone)
  selectedId = clone.id
  bump()
}

export function deleteWidget(id) {
  widgets.delete(id)
  if (selectedId === id) selectedId = null
  bump()
}

export function resetWidget(id) {
  const widget = widgets.get(id)
  if (!widget) return
  const fresh = createWidgetFromTemplate(widget.templateId)
  if (!fresh) return
  fresh.id = id
  widgets.set(id, fresh)
  bump()
}
