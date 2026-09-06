import { useEffect, useRef, useState } from 'react'

const TILE = 176
const GAP = 12
const STRIDE = TILE + GAP
const ROW_BUFFER = 3

// Windowing: only the rows near the scroll viewport are mounted, so a large
// wallpaper collection never holds hundreds of decoded tile images in memory.
// Tiles are plain positioned elements — no per-tile animation, so scrolling
// never triggers (re)mount animations.
export default function GalleryGrid({ records, renderTile, empty }) {
  const containerRef = useRef(null)
  const [viewport, setViewport] = useState({ scrollTop: 0, height: 0, width: 0 })

  useEffect(() => {
    const el = containerRef.current
    if (!el) return
    const measure = () =>
      setViewport((v) => ({ ...v, height: el.clientHeight, width: el.clientWidth }))
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  const cols = Math.max(1, Math.floor((viewport.width + GAP) / STRIDE))
  const rows = Math.ceil(records.length / cols)
  const totalHeight = Math.max(0, rows * STRIDE - GAP)

  if (records.length === 0) {
    return (
      <div className="gallery-grid" ref={containerRef}>
        {empty}
      </div>
    )
  }

  const firstRow = Math.max(0, Math.floor(viewport.scrollTop / STRIDE) - ROW_BUFFER)
  const lastRow = Math.min(rows - 1, Math.floor((viewport.scrollTop + viewport.height) / STRIDE) + ROW_BUFFER)
  const start = firstRow * cols
  const end = Math.min(records.length, (lastRow + 1) * cols)

  const items = []
  for (let i = start; i < end; i++) {
    const row = Math.floor(i / cols)
    const col = i % cols
    items.push(
      <div
        key={records[i].id}
        className="gallery-tile-slot"
        style={{ left: col * STRIDE, top: row * STRIDE }}
      >
        {renderTile(records[i])}
      </div>
    )
  }

  return (
    <div
      className="gallery-grid"
      ref={containerRef}
      onScroll={(e) => {
        const st = e.currentTarget.scrollTop
        setViewport((v) => ({ ...v, scrollTop: st }))
      }}
    >
      <div className="gallery-grid-inner" style={{ height: totalHeight }}>
        {items}
      </div>
    </div>
  )
}
