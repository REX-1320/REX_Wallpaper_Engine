import { useEffect, useRef } from 'react'
import { registerMedia, unregisterMedia } from './mediaBridge'
import { updateWallpaperProperty } from './store'

const FIT_BY_MODE = {
  Cover: 'cover',
  Fill: 'cover',
  Span: 'cover',
  'Fit Width': 'cover',
  Tile: 'cover',
  Contain: 'contain',
  'Fit Height': 'contain',
  Stretch: 'fill',
  Center: 'none',
}

export function mediaFit(mode) {
  return FIT_BY_MODE[mode] ?? 'cover'
}

// Normalizes perceived loudness through a Web Audio gain stage. The graph
// is created once per video element (createMediaElementSource cannot be
// undone), so toggling normalizeVolume only switches the gain computation.
function useNormalizedAudio(videoRef, wallpaper) {
  const normalizeRef = useRef(wallpaper.playback.normalizeVolume)
  normalizeRef.current = wallpaper.playback.normalizeVolume

  useEffect(() => {
    const video = videoRef.current
    if (!video || typeof AudioContext === 'undefined') return

    let ctx
    let analyser
    let gain
    let raf = 0
    let data
    try {
      ctx = new AudioContext()
      const src = ctx.createMediaElementSource(video)
      analyser = ctx.createAnalyser()
      analyser.fftSize = 512
      data = new Float32Array(analyser.fftSize)
      src.connect(analyser)
      gain = ctx.createGain()
      src.connect(gain).connect(ctx.destination)
    } catch {
      return
    }

    if (ctx.state === 'suspended') ctx.resume().catch(() => {})

    const TARGET_RMS = 0.15
    const tick = () => {
      if (normalizeRef.current) {
        analyser.getFloatTimeDomainData(data)
        let sum = 0
        for (let i = 0; i < data.length; i += 1) sum += data[i] * data[i]
        const rms = Math.sqrt(sum / data.length) || 1e-6
        gain.gain.value = Math.min(4, Math.max(0.05, TARGET_RMS / rms))
      } else {
        gain.gain.value = 1
      }
      raf = requestAnimationFrame(tick)
    }
    raf = requestAnimationFrame(tick)

    return () => {
      cancelAnimationFrame(raf)
      ctx.close().catch(() => {})
    }
  }, [wallpaper.id])
}

function VideoSource({ wallpaper }) {
  const videoRef = useRef(null)
  const loopRef = useRef(wallpaper.playback.loop)
  loopRef.current = wallpaper.playback.loop
  const { display, playback } = wallpaper

  useNormalizedAudio(videoRef, wallpaper)

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    registerMedia(wallpaper.id, video)
    const onEnded = () => {
      if (!loopRef.current) {
        updateWallpaperProperty(wallpaper.id, ['playback', 'paused'], true)
      }
    }
    video.addEventListener('ended', onEnded)
    return () => {
      video.removeEventListener('ended', onEnded)
      unregisterMedia(wallpaper.id)
    }
  }, [wallpaper.id])

  // Autoplay / random start / resume previous — applied once per source.
  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    const onLoaded = () => {
      if (playback.randomStart && video.duration > 0) {
        video.currentTime = Math.random() * video.duration
      } else if (playback.resumePrevious && playback.startPosition > 0) {
        video.currentTime = Math.min(playback.startPosition, video.duration || playback.startPosition)
      }
    }
    video.addEventListener('loadedmetadata', onLoaded)
    return () => video.removeEventListener('loadedmetadata', onLoaded)
  }, [wallpaper.id, playback.randomStart, playback.resumePrevious, playback.startPosition])

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    video.playbackRate = playback.speed
    video.volume = playback.volume / 100
    video.muted = playback.mute
    const shouldPlay = playback.autoplay && playback.playing && !playback.paused
    if (!shouldPlay) {
      video.pause()
      return
    }
    video.play().catch(() => {
      video.muted = true
      return video.play().catch(() => {})
    })
  }, [playback])

  return (
    <video
      ref={videoRef}
      className="wallpaper-media"
      src={wallpaper.fileUrl}
      loop={playback.loop}
      playsInline
      preload="auto"
      style={{ objectFit: mediaFit(display.mode) }}
    />
  )
}

export default function WallpaperMedia({ wallpaper }) {
  const { type, fileUrl, display } = wallpaper
  if (!fileUrl) return null

  if (type === 'IMAGE') {
    return (
      <img
        className="wallpaper-media"
        src={fileUrl}
        alt={wallpaper.info.name}
        draggable={false}
        style={{ objectFit: mediaFit(display.mode) }}
      />
    )
  }

  if (type === 'VIDEO') {
    return <VideoSource wallpaper={wallpaper} />
  }

  return null
}
