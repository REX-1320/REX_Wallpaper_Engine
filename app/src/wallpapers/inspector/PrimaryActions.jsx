import { Check, Pause, Play } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { applyWallpaper } from '../store'
import { usePauseState } from './usePauseState'

// The two actions that account for most inspector use: put this wallpaper on
// the desktop, or pause/resume the one that is already there. Both commands
// already existed underneath (applyWallpaper → DesktopApply; pause/resume via
// the runtime) — they just had no surface here.
export default function PrimaryActions({ wallpaper, applied }) {
  const canPause = applied && wallpaper.type === 'VIDEO'
  const { paused, pending, toggle } = usePauseState(canPause)

  return (
    <div className="insp-primary">
      {applied ? (
        <span className="insp-applied" aria-label="This wallpaper is on your desktop">
          <Check size={12} />
          On desktop
        </span>
      ) : (
        <Button className="flex-1" size="sm" onClick={() => applyWallpaper(wallpaper.id)}>
          Apply wallpaper
        </Button>
      )}
      {canPause ? (
        <Button className="flex-1" variant="secondary" size="sm" onClick={toggle} disabled={pending}>
          {paused ? <Play size={14} /> : <Pause size={14} />}
          {paused ? 'Resume' : 'Pause'}
        </Button>
      ) : null}
    </div>
  )
}
