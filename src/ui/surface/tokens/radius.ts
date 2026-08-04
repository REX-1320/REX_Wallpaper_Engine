/**
 * radius.ts
 *
 * Corner radius scale. Currently theme-independent (radius rarely differs
 * between light/dark), but kept as a `Record<ThemeName, ...>` so the generic
 * resolver in `tokens/index.ts` handles every category identically — future
 * themes can diverge without touching the resolver.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { RadiusTokens } from '../types/SurfaceToken';

const RADIUS: RadiusTokens = {
  xs: '2px',
  sm: '4px',
  md: '8px',
  lg: '12px',
  xl: '16px',
  full: '9999px',
};

export const radiusTokens: Record<ThemeName, RadiusTokens> = {
  light: RADIUS,
  dark: RADIUS,
  highContrast: RADIUS,
};
