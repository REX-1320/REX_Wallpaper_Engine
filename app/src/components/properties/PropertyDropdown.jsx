import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import PropertyRow from './PropertyRow'

function normalize(options) {
  return options.map((option) =>
    typeof option === 'string' ? { value: option, label: option } : option
  )
}

export default function PropertyDropdown({ label, description, tooltip, value, options, onChange }) {
  const items = normalize(options)
  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <Select value={value} onValueChange={onChange}>
        <SelectTrigger size="sm" className="h-7 w-28 text-xs">
          <SelectValue />
        </SelectTrigger>
        <SelectContent>
          {items.map((item) => (
            <SelectItem key={item.value} value={item.value}>
              {item.label}
            </SelectItem>
          ))}
        </SelectContent>
      </Select>
    </PropertyRow>
  )
}
