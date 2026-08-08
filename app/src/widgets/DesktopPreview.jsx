import { useLayoutEffect, useRef, useState } from 'react'
import { AnimatePresence } from 'motion/react'
import { selectWidget, useWidgetStore } from './store'
import WidgetVisual from './WidgetVisual'

export default function DesktopPreview() {
  const { widgets, selectedId } = useWidgetStore()
  const previewRef = useRef(null)
  const [bounds, setBounds] = useState({ width: 0, height: 0 })

  useLayoutEffect(() => {
    const el = previewRef.current
    if (!el) return
    const measure = () => setBounds({ width: el.clientWidth, height: el.clientHeight })
    measure()
    const observer = new ResizeObserver(measure)
    observer.observe(el)
    return () => observer.disconnect()
  }, [])

  return (
    <div className="desktop-preview" ref={previewRef} onClick={() => selectWidget(null)}>
      <span className="desktop-preview-label">Desktop Preview</span>
      <AnimatePresence>
        {widgets.map((widget) => (
          <WidgetVisual
            key={widget.id}
            widget={widget}
            bounds={bounds}
            selected={widget.id === selectedId}
            onSelect={(e) => {
              e?.stopPropagation?.()
              selectWidget(widget.id)
            }}
          />
        ))}
      </AnimatePresence>
    </div>
  )
}
