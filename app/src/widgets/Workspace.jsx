import { useEffect } from 'react'
import { seedWidgets } from './store'
import WidgetsCollection from './WidgetsCollection'
import WidgetDetails from './WidgetDetails'
import DesktopPreview from './DesktopPreview'
import WidgetInspector from './inspector/Inspector'
import './inspector/inspector.css'

export default function WidgetsWorkspace() {
  useEffect(() => {
    seedWidgets()
  }, [])

  return (
    <div className="widgets-frame dark">
      <WidgetsCollection />
      <WidgetDetails />
      <div className="widgets-workspace">
        <div className="widgets-preview">
          <DesktopPreview />
        </div>
        <WidgetInspector />
      </div>
    </div>
  )
}
