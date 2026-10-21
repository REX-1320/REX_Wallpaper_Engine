import { useRef } from 'react'

// A drag-to-resize strip, VS Code style.
//
// The handle reports a DELTA from the moment the drag started, so the owner
// can keep the "value at drag start" and apply clamping itself. onCommit fires
// once at the end of the drag — that is the only moment worth persisting
// (writing on every pointermove would hammer the state file).
export default function ResizeHandle({ axis = 'x', onResize, onCommit, className = '', label = 'Resize' }) {
  const startRef = useRef(null)

  const onPointerDown = (event) => {
    if (event.button !== 0) return
    event.preventDefault()
    event.stopPropagation()
    const origin = axis === 'x' ? event.clientX : event.clientY
    startRef.current = origin
    const target = event.currentTarget
    try {
      target.setPointerCapture(event.pointerId)
    } catch {
      /* capture is best-effort */
    }

    const move = (ev) => {
      const current = axis === 'x' ? ev.clientX : ev.clientY
      onResize?.(current - origin)
    }
    const up = () => {
      window.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', up)
      window.removeEventListener('pointercancel', up)
      try {
        target.releasePointerCapture?.(event.pointerId)
      } catch {
        /* already released */
      }
      startRef.current = null
      onCommit?.()
    }
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', up)
    window.addEventListener('pointercancel', up)
  }

  const onKeyDown = (event) => {
    const step = event.shiftKey ? 40 : 12
    if (axis === 'x' && (event.key === 'ArrowLeft' || event.key === 'ArrowRight')) {
      event.preventDefault()
      onResize?.(event.key === 'ArrowLeft' ? -step : step)
      onCommit?.()
    }
    if (axis === 'y' && (event.key === 'ArrowUp' || event.key === 'ArrowDown')) {
      event.preventDefault()
      onResize?.(event.key === 'ArrowUp' ? -step : step)
      onCommit?.()
    }
  }

  return (
    <div
      className={`resize-handle resize-handle-${axis} ${className}`}
      role="separator"
      aria-orientation={axis === 'x' ? 'vertical' : 'horizontal'}
      aria-label={label}
      tabIndex={0}
      onPointerDown={onPointerDown}
      onKeyDown={onKeyDown}
    />
  )
}
