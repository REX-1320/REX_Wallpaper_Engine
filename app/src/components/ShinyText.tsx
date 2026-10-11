import React from 'react';
import './ShinyText.css';

interface ShinyTextProps {
  text: string;
  disabled?: boolean;
  speed?: number;
  className?: string;
  color?: string;
  shineColor?: string;
  spread?: number;
  yoyo?: boolean;
  pauseOnHover?: boolean;
  direction?: 'left' | 'right';
  delay?: number;
}

// Static shimmer: a one-time CSS gradient sweep instead of the previous
// useAnimationFrame loop that fired 160x/sec and forced Chromium to
// re-rasterize the gradient text every frame (GPU contention with the
// wallpaper's D3D12 Present). The shine is baked into a background-position
// keyframe that runs once via CSS animation — Chromium handles it on the
// compositor thread with near-zero cost.
const ShinyText: React.FC<ShinyTextProps> = ({
  text,
  disabled = false,
  className = '',
  color = '#b5b5b5',
  shineColor = '#ffffff',
  spread = 120,
}) => {
  const gradientStyle: React.CSSProperties = {
    backgroundImage: `linear-gradient(${spread}deg, ${color} 0%, ${color} 35%, ${shineColor} 50%, ${color} 65%, ${color} 100%)`,
    backgroundSize: '200% auto',
    WebkitBackgroundClip: 'text',
    backgroundClip: 'text',
    WebkitTextFillColor: 'transparent',
  };

  return (
    <span
      className={`shiny-text ${disabled ? 'shiny-static' : ''} ${className}`.trim()}
      style={gradientStyle}
    >
      {text}
    </span>
  );
};

export default ShinyText;
