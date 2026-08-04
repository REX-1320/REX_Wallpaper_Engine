/**
 * blur.ts
 *
 * Blur radii for the future glass-family materials (Glass, Acrylic, Mica,
 * Liquid Glass). Phase 1 is architecture-only: these values are defined and
 * resolved, but NO rendering path applies them yet.
 *
 * The zeroed variant is used by the token resolver when the global
 * performance mode is `low`, guaranteeing glass materials can degrade
 * gracefully without each material knowing about performance profiles.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { BlurTokens } from '../types/SurfaceToken';

const BLUR: BlurTokens = {
  sm: '8px',
  md: '16px',
  lg: '32px',
  xl: '64px',
};

/** All radii disabled — the `performance="low"` degradation target. */
export const zeroBlurTokens: BlurTokens = {
  sm: '0px',
  md: '0px',
  lg: '0px',
  xl: '0px',
};

export const blurTokens: Record<ThemeName, BlurTokens> = {
  light: BLUR,
  dark: BLUR,
  highContrast: BLUR,
};
