/**
 * opacity.ts
 *
 * Opacity scale. These values feed future glass-family materials
 * (surface.glass, surface.translucent) and the text/border hierarchy.
 * Phase 1 consumes them for the transparent text hierarchy only.
 *
 * Values are raw 0..1 numbers — materials are responsible for applying
 * them (e.g. via rgba() composition or CSS alpha), never hard-coded.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { OpacityTokens } from '../types/SurfaceToken';

const LIGHT: OpacityTokens = {
  surface: { solid: 1, translucent: 0.92, glass: 0.7 },
  border: { hairline: 0.1, default: 0.16 },
  overlay: { subtle: 0.25, scrim: 0.5 },
  text: { primary: 1, secondary: 0.65, disabled: 0.4 },
};

const DARK: OpacityTokens = {
  surface: { solid: 1, translucent: 0.9, glass: 0.7 },
  border: { hairline: 0.12, default: 0.2 },
  overlay: { subtle: 0.3, scrim: 0.6 },
  text: { primary: 1, secondary: 0.7, disabled: 0.45 },
};

export const opacityTokens: Record<ThemeName, OpacityTokens> = {
  light: LIGHT,
  dark: DARK,
  // High contrast must prioritize legibility: opaque text, strong borders.
  highContrast: {
    surface: { solid: 1, translucent: 1, glass: 1 },
    border: { hairline: 0.6, default: 0.8 },
    overlay: { subtle: 0.5, scrim: 0.8 },
    text: { primary: 1, secondary: 1, disabled: 0.75 },
  },
};
