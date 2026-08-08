import { useRef } from 'react'
import PropertyRow from './PropertyRow'

export default function PropertyColor({ label, description, tooltip, value, onChange }) {
  const inputRef = useRef(null)

  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <button
        type="button"
        aria-label={`Pick ${label}`}
        className="relative size-7 shrink-0 overflow-hidden rounded-md border border-border/60 shadow-sm"
        style={{ background: value }}
        onClick={() => (inputRef.current?.showPicker?.() ?? inputRef.current?.click())}
      >
        <input
          ref={inputRef}
          type="color"
          className="absolute inset-0 size-full cursor-pointer opacity-0"
          value={value}
          onChange={(e) => onChange(e.target.value)}
        />
      </button>
    </PropertyRow>
  )
}
