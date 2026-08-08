import { useEffect, useRef } from 'react'
import { AnimatePresence } from 'motion/react'
import { seedWallpapers, updateWallpaperProperty, useWallpaperStore } from './store'
import WallpaperVisual from './WallpaperVisual'
import MediaControlsOverlay from './MediaControlsOverlay'
import './inspector/wallpaper.css'

export default function WallpaperPreview() {
  const canvasRef = useRef(null)
  const { wallpapers, selectedId } = useWallpaperStore()

  useEffect(() => {
    seedWallpapers()
  }, [])

  const selected = wallpapers.find((w) => w.id === selectedId) ?? null

  const togglePlay = () => {
    if (!selected) return
    const playing = selected.playback.playing && !selected.playback.paused
    updateWallpaperProperty(selected.id, ['playback', 'playing'], !playing)
    updateWallpaperProperty(selected.id, ['playback', 'paused'], playing)
  }

  return (
    <div
      ref={canvasRef}
      className="wallpaper-canvas"
      onClick={togglePlay}
      title="Click to play or pause"
    >
      <AnimatePresence>
        {wallpapers.map((wallpaper) => (
          <WallpaperVisual
            key={wallpaper.id}
            wallpaper={wallpaper}
            selected={wallpaper.id === selectedId}
          />
        ))}
      </AnimatePresence>
      <MediaControlsOverlay canvasRef={canvasRef} />
    </div>
  )
}
