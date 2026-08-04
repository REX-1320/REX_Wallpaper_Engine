/**
 * animation.ts
 *
 * Motion tokens (durations + easings). Phase 1 defines them so future
 * material and component animations share one rhythm; the `animationLevel`
 * context flag (full / reduced / none) is the switch components will use
 * to respect user motion preferences.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { AnimationTokens } from '../types/SurfaceToken';

const ANIMATION: AnimationTokens = {
  duration: {
    instant: '80ms',
    fast: '150ms',
    normal: '250ms',
    slow: '400ms',
    slower: '600ms',
  },
  easing: {
    standard: 'cubic-bezier(0.2, 0, 0, 1)',
    decelerate: 'cubic-bezier(0, 0, 0.2, 1)',
    accelerate: 'cubic-bezier(0.4, 0, 1, 1)',
    spring: 'cubic-bezier(0.34, 1.56, 0.64, 1)',
  },
};

export const animationTokens: Record<ThemeName, AnimationTokens> = {
  light: ANIMATION,
  dark: ANIMATION,
  highContrast: ANIMATION,
};
