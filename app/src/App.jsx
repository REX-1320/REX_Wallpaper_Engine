import { useState } from 'react'
import { motion } from 'motion/react'
import SidebarLayout from './components/SidebarLayout'
import ShadcnGallery from './ShadcnGallery'
import SpecularButton from './components/SpecularButton'
import WidgetsWorkspace from './widgets/Workspace'
import WallpaperGallery from './wallpapers/WallpaperGallery'
import WallpaperInspector from './wallpapers/inspector/Inspector'

function greeting() {
  const h = new Date().getHours()
  if (h < 5) return 'Good night'
  if (h < 12) return 'Good morning'
  if (h < 17) return 'Good afternoon'
  if (h < 21) return 'Good evening'
  return 'Good night'
}

function App() {
  const [tab, setTab] = useState('Wallpapers')

  return (
    <SidebarLayout active={tab} onSelect={setTab}>
      <motion.section
        className="content-section"
        key={tab}
        initial={{ opacity: 0, y: 16 }}
        animate={{ opacity: 1, y: 0 }}
        transition={{ type: 'spring', stiffness: 260, damping: 24 }}
      >
        <h2>{tab === 'Home' ? `${greeting()}!` : tab}</h2>
        {tab === 'shadcn' ? (
          <div className="tab-panel">
            <ShadcnGallery />
          </div>
        ) : tab === 'Home' ? (
          <div className="tab-panel">
            <div className="home-panel">
              <p>{greeting()}! Welcome back to your Home tab.</p>
              <SpecularButton
                size="lg"
                radius={999}
                baseColor="#3a3a3a"
                lineColor="#ffffff"
                speed={0.4}
              >
                Get Started
              </SpecularButton>
            </div>
          </div>
        ) : tab === 'Widgets' ? (
          <WidgetsWorkspace />
        ) : tab === 'Wallpapers' ? (
          <div className="tab-panel plain">
            <div className="wallpaper-grid">
              <div className="wallpaper-cube w1">
                <WallpaperGallery />
              </div>
              <div className="wallpaper-cube w2">
                <WallpaperInspector />
              </div>
            </div>
          </div>
        ) : null}
      </motion.section>
    </SidebarLayout>
  )
}

export default App
