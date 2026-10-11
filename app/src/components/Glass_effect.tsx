import type { CSSProperties } from 'react'
import './Glass_effect.css'

export interface Glass_effectProps {
  blur?: number
  fillOpacity?: number
  saturation?: number
  brightness?: number
  radius?: number
  dark?: boolean
  className?: string
}

export default function Glass_effect({
  blur,
  fillOpacity,
  saturation,
  brightness,
  radius,
  dark = false,
  className = ''
}: Glass_effectProps) {
  const style = {
    ...(blur !== undefined ? { '--glass-blur': `${blur}px` } : {}),
    ...(fillOpacity !== undefined ? { '--glass-fill-opacity': fillOpacity } : {}),
    ...(saturation !== undefined ? { '--glass-saturation': saturation } : {}),
    ...(brightness !== undefined ? { '--glass-brightness': brightness } : {}),
    ...(radius !== undefined ? { '--glass-radius': `${radius}px` } : {}),
  } as CSSProperties

  return (
    <div
      className={`glass-effect ${dark ? 'dark' : ''} ${className}`.trim()}
      style={style}
      aria-hidden="true"
    />
  )
}
