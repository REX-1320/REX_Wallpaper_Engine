import PropertyDropdown from './PropertyDropdown'

export default function PropertyMonitorPicker({
  label,
  description,
  tooltip,
  value,
  onChange,
  monitors = ['Monitor 1', 'Monitor 2'],
  custom = 'Custom',
}) {
  return (
    <PropertyDropdown
      label={label}
      description={description}
      tooltip={tooltip}
      value={value}
      options={[...monitors, 'All Monitors', custom]}
      onChange={onChange}
    />
  )
}
