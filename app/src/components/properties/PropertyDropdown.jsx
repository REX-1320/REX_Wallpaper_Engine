import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectLabel,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import PropertyRow from './PropertyRow'

function normalize(options) {
  return options.map((option) =>
    typeof option === 'string' ? { value: option, label: option } : option
  )
}

// Options may carry a `group` ("Blur", "Basic", ...). Grouped options are
// rendered as labelled sections in the list — a flat 20-item dropdown is
// unreadable.
function groupOptions(options) {
  const groups = []
  for (const option of options) {
    const name = option.group ?? ''
    let group = groups.find((g) => g.name === name)
    if (!group) {
      group = { name, items: [] }
      groups.push(group)
    }
    group.items.push(option)
  }
  return groups
}

export default function PropertyDropdown({ label, description, tooltip, value, options, onChange }) {
  const items = normalize(options)
  const groups = groupOptions(items)
  const grouped = groups.some((group) => group.name)

  return (
    <PropertyRow label={label} description={description} tooltip={tooltip}>
      <Select value={value} onValueChange={onChange}>
        <SelectTrigger size="sm" className="h-7 w-28 text-xs">
          <SelectValue />
        </SelectTrigger>
        <SelectContent align="center">
          {grouped
            ? groups.map((group) => (
                <SelectGroup key={group.name || 'default'}>
                  {group.name ? <SelectLabel>{group.name}</SelectLabel> : null}
                  {group.items.map((item) => (
                    <SelectItem key={item.value} value={item.value}>
                      {item.label}
                    </SelectItem>
                  ))}
                </SelectGroup>
              ))
            : items.map((item) => (
                <SelectItem key={item.value} value={item.value}>
                  {item.label}
                </SelectItem>
              ))}
        </SelectContent>
      </Select>
    </PropertyRow>
  )
}
