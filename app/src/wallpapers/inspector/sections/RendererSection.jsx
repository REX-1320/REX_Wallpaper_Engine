import { Box } from 'lucide-react'
import PropertySection from '@/components/properties/PropertySection'
import PropertyGroup from '@/components/properties/PropertyGroup'
import PropertyDropdown from '@/components/properties/PropertyDropdown'
import PropertyToggle from '@/components/properties/PropertyToggle'
import PropertySlider from '@/components/properties/PropertySlider'
import PropertyText from '@/components/properties/PropertyText'
import PropertyTimeline from '@/components/properties/PropertyTimeline'
import PropertyFilePicker from '@/components/properties/PropertyFilePicker'
import PropertyNumber from '@/components/properties/PropertyNumber'
import {
  AUDIO_SOURCES,
  BROWSER_ENGINES,
  CAMERAS,
  CODECS,
  FFT_SIZES,
  LIGHTINGS,
  QUALITIES,
  TRANSITIONS,
  TYPE_META,
  parseDuration,
} from '../../registry'
import { updateWallpaperProperty } from '../../store'

export default function RendererSection({ wallpaper }) {
  const { renderer, type, info } = wallpaper
  const meta = TYPE_META[type]
  const duration = parseDuration(info.duration)
  const set = (key) => (value) => updateWallpaperProperty(wallpaper.id, ['renderer', key], value)

  return (
    <PropertySection title={`Wallpaper Type · ${meta.label}`} icon={<Box size={15} />}>
      {type === 'IMAGE' && (
        <>
          <PropertySlider label="Zoom" value={renderer.zoom} onChange={set('zoom')} min={25} max={400} suffix="%" />
          <PropertySlider label="Pan" value={renderer.pan} onChange={set('pan')} min={-100} max={100} suffix="%" />
          <PropertyToggle label="Ken Burns" checked={renderer.kenBurns} onChange={set('kenBurns')} />
          <PropertyToggle label="Slideshow" checked={renderer.slideshow} onChange={set('slideshow')} />
          <PropertyDropdown
            label="Transition"
            value={renderer.transition}
            options={TRANSITIONS}
            onChange={set('transition')}
          />
        </>
      )}
      {type === 'VIDEO' && (
        <>
          <PropertyGroup title="Video Stream">
            <PropertyDropdown label="Codec" value={renderer.codec} options={CODECS} onChange={set('codec')} />
            <PropertyToggle label="Hardware Decode" checked={renderer.hardwareDecode} onChange={set('hardwareDecode')} />
            <PropertyToggle label="Frame Skip" checked={renderer.frameSkip} onChange={set('frameSkip')} />
          </PropertyGroup>
          <PropertyToggle label="Loop" checked={renderer.loop} onChange={set('loop')} />
          <PropertyTimeline
            label="Start Position"
            value={renderer.startPosition}
            onChange={set('startPosition')}
            min={0}
            max={duration}
          />
          <PropertyFilePicker
            label="Subtitle"
            value={renderer.subtitle || 'None'}
            onChange={set('subtitle')}
            accept=".srt,.vtt,.ass"
          />
        </>
      )}
      {type === 'WEB' && (
        <>
          <PropertyGroup title="Page">
            <PropertyText label="URL" value={renderer.url} onChange={set('url')} />
            <PropertyDropdown
              label="Browser Engine"
              value={renderer.browserEngine}
              options={BROWSER_ENGINES}
              onChange={set('browserEngine')}
            />
          </PropertyGroup>
          <PropertyToggle label="Reload" checked={renderer.reload} onChange={set('reload')} />
          <PropertyToggle label="Dev Tools" checked={renderer.devTools} onChange={set('devTools')} />
          <PropertyToggle label="Cache" checked={renderer.cache} onChange={set('cache')} />
          <PropertyToggle label="JavaScript" checked={renderer.javascript} onChange={set('javascript')} />
          <PropertySlider label="Zoom" value={renderer.zoom} onChange={set('zoom')} min={25} max={400} suffix="%" />
        </>
      )}
      {type === 'SCENE' && (
        <>
          <PropertyGroup title="Scene">
            <PropertyDropdown label="Camera" value={renderer.camera} options={CAMERAS} onChange={set('camera')} />
            <PropertyDropdown label="Lighting" value={renderer.lighting} options={LIGHTINGS} onChange={set('lighting')} />
            <PropertyDropdown label="Quality" value={renderer.quality} options={QUALITIES} onChange={set('quality')} />
          </PropertyGroup>
          <PropertyToggle label="Post Processing" checked={renderer.postProcessing} onChange={set('postProcessing')} />
          <PropertyToggle label="Particles" checked={renderer.particles} onChange={set('particles')} />
          <PropertyToggle label="Physics" checked={renderer.physics} onChange={set('physics')} />
        </>
      )}
      {type === 'SHADER' && (
        <>
          <PropertyFilePicker
            label="Shader File"
            value={renderer.shaderFile}
            onChange={set('shaderFile')}
            accept=".glsl,.hlsl,.frag,.vert,.comp,.shader"
          />
          <PropertyText label="Uniforms" value={renderer.uniforms} onChange={set('uniforms')} mono />
          <PropertyText label="Texture Inputs" value={renderer.textureInputs} onChange={set('textureInputs')} mono />
          <PropertyNumber label="FPS" value={renderer.fps} onChange={set('fps')} min={15} max={240} suffix=" fps" />
          <PropertyText label="Canvas Size" value={renderer.canvasSize} onChange={set('canvasSize')} />
        </>
      )}
      {type === 'AUDIO_VISUALIZER' && (
        <>
          <PropertyGroup title="Analysis">
            <PropertyDropdown
              label="Audio Source"
              value={renderer.audioSource}
              options={AUDIO_SOURCES}
              onChange={set('audioSource')}
            />
            <PropertyDropdown label="FFT Size" value={renderer.fftSize} options={FFT_SIZES} onChange={set('fftSize')} />
          </PropertyGroup>
          <PropertyToggle label="Bars" checked={renderer.bars} onChange={set('bars')} />
          <PropertyToggle label="Wave" checked={renderer.wave} onChange={set('wave')} />
          <PropertyToggle label="Spectrum" checked={renderer.spectrum} onChange={set('spectrum')} />
          <PropertySlider
            label="Sensitivity"
            value={renderer.sensitivity}
            onChange={set('sensitivity')}
            min={0}
            max={100}
            suffix="%"
          />
          <PropertyToggle label="Peak Hold" checked={renderer.peakHold} onChange={set('peakHold')} />
          <PropertyToggle label="Custom Colors" checked={renderer.customColors} onChange={set('customColors')} />
        </>
      )}
    </PropertySection>
  )
}
