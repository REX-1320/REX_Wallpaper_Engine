import { useEffect, useState } from 'react'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyRow from '@/components/properties/PropertyRow'
import paiintMark from '../assets/paiint-mark.png'
import './about.css'

// The About page (sidebar > About). The legal texts are read from the files
// that shipped with this build (see app:read-legal in main.cjs), so the page
// cannot drift from the package.
export default function AboutPage() {
  const [version, setVersion] = useState('')
  const [doc, setDoc] = useState(null)
  const [error, setError] = useState('')

  useEffect(() => {
    window.paiintDesktop?.appState?.get?.('version').then((v) => setVersion(v ?? '')).catch(() => {})
  }, [])

  const openDoc = async (target) => {
    setError('')
    try {
      const res = await window.paiintDesktop?.readLegal?.(target)
      if (res?.ok) setDoc({ title: res.title, text: res.text })
      else setError('That document could not be opened.')
    } catch {
      setError('That document could not be opened.')
    }
  }

  return (
    <div className="about-page">
      <div className="about-inner">
        <div className="about-head">
          <img className="about-mark" src={paiintMark} alt="" aria-hidden="true" />
          <div className="about-id">
            <span className="about-name">Paiint</span>
            <span className="about-tagline">Native wallpaper engine for Windows</span>
          </div>
          <span className="about-version">{version ? `v${version}` : '—'}</span>
        </div>

        <p className="about-blurb">
          Video, HDR and still wallpapers, rendered behind your icons. Native
          C++/D3D12 runtime. Quit, and the last frame stays.
        </p>

        <PropertyGroup>
          <PropertyRow label="Publisher">
            <span>MoiNeX</span>
          </PropertyRow>
          <PropertyRow label="Engine">
            <span className="pl-mono">paiint-wallpaper-runtime</span>
          </PropertyRow>
          <PropertyRow label="Licence">
            <span>Proprietary</span>
          </PropertyRow>
          <PropertyRow label="Copyright">
            <span>© 2026 MoiNeX</span>
          </PropertyRow>
        </PropertyGroup>

        <p className="about-note">
          <strong>Privacy:</strong> No telemetry. No network. Wallpapers, playlists
          and settings stay on this PC.
        </p>
        <p className="about-note">
          Bundles FFmpeg (LGPL-2.1) and other open-source components. Every licence
          text ships with the app.
        </p>

        <div className="about-legal">
          <Button variant="outline" size="sm" onClick={() => openDoc('eula')}>
            Licence agreement
          </Button>
          <Button variant="outline" size="sm" onClick={() => openDoc('privacy')}>
            Privacy policy
          </Button>
          <Button variant="outline" size="sm" onClick={() => openDoc('notices')}>
            Third-party notices
          </Button>
          <Button variant="outline" size="sm" onClick={() => openDoc('jsLicenses')}>
            JavaScript licences
          </Button>
        </div>
        {error ? <p className="about-error">{error}</p> : null}
      </div>

      <Dialog open={!!doc} onOpenChange={(open) => { if (!open) setDoc(null) }}>
        <DialogContent className="about-legal-dialog">
          <DialogHeader>
            <DialogTitle>{doc?.title ?? ''}</DialogTitle>
            <DialogDescription>
              The exact text that shipped with this build.
            </DialogDescription>
          </DialogHeader>
          <pre className="about-legal-text">{doc?.text ?? ''}</pre>
        </DialogContent>
      </Dialog>
    </div>
  )
}
