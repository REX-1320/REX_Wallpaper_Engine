import { CalendarClock } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertyTimeline from '@/components/properties/PropertyTimeline'
import PropertyToggle from '@/components/properties/PropertyToggle'
import { ACTIVE_TIMES, DAY_SCHEDULES } from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function ScheduleSection({ wallpaper }) {
  const { schedule } = wallpaper
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['schedule', key], value)

  return (
    <PropertySection title="Scheduling" icon={<CalendarClock size={15} />}>
      <PropertyDropdown
        label="Active Time"
        value={schedule.activeTime}
        options={ACTIVE_TIMES}
        onChange={set('activeTime')}
      />
      <PropertyTimeline
        label="Start Time"
        value={schedule.startTime}
        onChange={set('startTime')}
        min={0}
        max={24}
        step={0.25}
      />
      <PropertyTimeline
        label="End Time"
        value={schedule.endTime}
        onChange={set('endTime')}
        min={0}
        max={24}
        step={0.25}
      />
      <PropertyDropdown
        label="Day Schedule"
        value={schedule.daySchedule}
        options={DAY_SCHEDULES}
        onChange={set('daySchedule')}
      />
      <PropertyToggle label="Gaming Profile" checked={schedule.gamingProfile} onChange={set('gamingProfile')} />
      <PropertyToggle label="Battery Profile" checked={schedule.batteryProfile} onChange={set('batteryProfile')} />
      <PropertyToggle label="Sleep Profile" checked={schedule.sleepProfile} onChange={set('sleepProfile')} />
    </PropertySection>
  )
}
