import { Input } from '@/components/ui/input'
import PropertyRow from './PropertyRow'

export default function PropertyText({
  label,
  description,
  tooltip,
  value,
  readOnly = false,
  mono = false,
  onChange,
}) {
  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <Input
        className={`h-7 w-28 text-xs${mono ? ' font-mono' : ''}`}
        value={value}
        readOnly={readOnly}
        onChange={onChange ? (e) => onChange(e.target.value) : undefined}
      />
    </PropertyRow>
  )
}
