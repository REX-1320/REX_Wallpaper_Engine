/**
 * shadow.ts
 *
 * Elevation-based shadow scale. Each elevation step (0..5) is a complete,
 * serializable spec; `shadowToCss` is the ONLY place that knows how to turn
 * a spec into a CSS value. Keeping the serializer in the token layer means
 * no component or material ever formats a shadow string by hand.
 *
 * Architectural decision: shadow color is carried inside the spec (already
 * theme-resolved) so materials stay theme-agnostic — they just render what
 * the resolver handed them.
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { Elevation } from '../core/SurfaceTypes';
import type { ShadowSpec, ShadowTokens } from '../types/SurfaceToken';

const LIGHT: ShadowTokens = {
  0: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#1A1A1A' },
  1: { offsetX: 0, offsetY: 1, blur: 2, spread: 0, color: '#1A1A1A' },
  2: { offsetX: 0, offsetY: 2, blur: 8, spread: 0, color: '#1A1A1A' },
  3: { offsetX: 0, offsetY: 4, blur: 16, spread: 0, color: '#000000' },
  4: { offsetX: 0, offsetY: 8, blur: 24, spread: 0, color: '#000000' },
  5: { offsetX: 0, offsetY: 12, blur: 40, spread: 2, color: '#000000' },
};

const DARK: ShadowTokens = {
  0: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  1: { offsetX: 0, offsetY: 1, blur: 2, spread: 0, color: '#000000' },
  2: { offsetX: 0, offsetY: 2, blur: 8, spread: 0, color: '#000000' },
  3: { offsetX: 0, offsetY: 4, blur: 16, spread: 0, color: '#000000' },
  4: { offsetX: 0, offsetY: 8, blur: 24, spread: 0, color: '#000000' },
  5: { offsetX: 0, offsetY: 12, blur: 40, spread: 2, color: '#000000' },
};

// High contrast: shadows carry no visual information, render them off.
const HIGH_CONTRAST: ShadowTokens = {
  0: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  1: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  2: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  3: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  4: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
  5: { offsetX: 0, offsetY: 0, blur: 0, spread: 0, color: '#000000' },
};

export const shadowTokens: Record<ThemeName, ShadowTokens> = {
  light: LIGHT,
  dark: DARK,
  highContrast: HIGH_CONTRAST,
};

/**
 * Serializes a shadow spec into a CSS `box-shadow` value.
 * The only place in the codebase that formats shadows.
 */
export function shadowToCss(spec: ShadowSpec): string {
  return `${spec.offsetX}px ${spec.offsetY}px ${spec.blur}px ${spec.spread}px ${spec.color}`;
}

/**
 * The list of valid elevation steps — the exhaustive key set of
 * `ShadowTokens`. Kept in the token layer so future performance profiles
 * can iterate elevations generically.
 */
export const ELEVATIONS: readonly Elevation[] = [0, 1, 2, 3, 4, 5];
