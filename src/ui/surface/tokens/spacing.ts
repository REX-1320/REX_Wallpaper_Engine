/**
 * spacing.ts
 *
 * Spacing scale for future REX components (padding, gaps, insets).
 * Exposed through the surface context so every component consumes the
 * same rhythm — no magic numbers anywhere in component code.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { SpacingTokens } from '../types/SurfaceToken';

const SPACING: SpacingTokens = {
  xs: '4px',
  sm: '8px',
  md: '12px',
  lg: '16px',
  xl: '24px',
  xxl: '32px',
};

export const spacingTokens: Record<ThemeName, SpacingTokens> = {
  light: SPACING,
  dark: SPACING,
  highContrast: SPACING,
};
