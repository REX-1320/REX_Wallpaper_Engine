import type { CSSProperties } from 'react'
import './MicaBackdrop.css'

export interface MicaBackdropProps {
  variant?: 'mica' | 'acrylic'
  tint?: string
  tintOpacity?: number
  dark?: boolean
  colors?: string[]
  className?: string
}

const LIGHT_COLORS = ['#a9bff0', '#f0c8d8', '#bfe8d0', '#e8dcb8']
const DARK_COLORS = ['#262b4d', '#4d2637', '#1f4038', '#3a2e1f']

export default function MicaBackdrop({
  variant = 'mica',
  tint,
  tintOpacity = 0.55,
  dark = false,
  colors,
  className = ''
}: MicaBackdropProps) {
  const palette = colors ?? (dark ? DARK_COLORS : LIGHT_COLORS)

  return (
    <div
      className={`mica-backdrop ${variant} ${dark ? 'dark' : 'light'} ${className}`.trim()}
      style={{ '--mica-tint': tint ?? (dark ? '#202020' : '#f3f3f3') } as CSSProperties}
      aria-hidden="true"
    >
      <div className="mica-blobs">
        {palette.map((color, i) => (
          <span
            key={i}
            className="mica-blob"
            style={{ '--mica-blob': color } as CSSProperties}
          />
        ))}
      </div>
      <div
        className="mica-frost"
        style={{ '--mica-tint-opacity': tintOpacity } as CSSProperties}
      />
      {variant === 'acrylic' && <div className="mica-noise" />}
    </div>
  )
}
