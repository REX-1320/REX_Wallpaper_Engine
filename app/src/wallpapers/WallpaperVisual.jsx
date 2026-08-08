import { motion } from 'motion/react'
import { TYPE_META } from './registry'
import WallpaperMedia from './WallpaperMedia'

export function buildFilter(appearance) {
  return [
    `brightness(${(appearance.brightness * appearance.exposure) / 100}%)`,
    `contrast(${appearance.contrast}%)`,
    `saturate(${(appearance.saturation * appearance.vibrance) / 100}%)`,
    `hue-rotate(${appearance.hue + (appearance.temperature - 50) * 0.6}deg)`,
    `grayscale(${appearance.grayscale}%)`,
    `invert(${appearance.invert}%)`,
    `blur(${appearance.blur}px)`,
  ].join(' ')
}

export default function WallpaperVisual({ wallpaper, selected }) {
  const { appearance, display } = wallpaper
  const meta = TYPE_META[wallpaper.type]
  const Icon = meta.icon

  const style = {
    background: meta.gradient,
    opacity: appearance.opacity / 100,
    filter: buildFilter(appearance),
    boxShadow: appearance.vignette > 0
      ? `inset 0 0 120px rgba(0, 0, 0, ${appearance.vignette / 100})`
      : undefined,
    transform: `translate(${display.offsetX}px, ${display.offsetY}px) rotate(${display.rotation}deg) scale(${display.scale / 100}) scale(${display.flipH ? -1 : 1}, ${display.flipV ? -1 : 1})`,
  }

  return (
    <motion.div
      className={`wallpaper-visual${selected ? ' selected' : ''}`}
      style={style}
      initial={{ opacity: 0 }}
      animate={{ opacity: appearance.opacity / 100 }}
      exit={{ opacity: 0, scale: 0.9 }}
      transition={{ type: 'spring', stiffness: 320, damping: 28 }}
    >
      <WallpaperMedia wallpaper={wallpaper} />
      {!wallpaper.fileUrl ? <Icon size={26} /> : null}
    </motion.div>
  )
}
