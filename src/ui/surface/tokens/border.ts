/**
 * border.ts
 *
 * Border stroke widths. Border *colors* live in `colors.ts` (they are
 * theme-dependent); widths are structural and shared across themes.
 * Colors are separated from widths so that materials can combine any
 * width with any color without duplicating definitions.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { BorderTokens } from '../types/SurfaceToken';

const BORDER: BorderTokens = {
  width: {
    hairline: '1px',
    thin: '2px',
    medium: '3px',
  },
};

export const borderTokens: Record<ThemeName, BorderTokens> = {
  light: BORDER,
  dark: BORDER,
  highContrast: BORDER,
};
