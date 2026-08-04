/**
 * SurfaceToken.ts
 *
 * Defines the contract for every design token category used by the REX
 * Surface System. Tokens are the single source of truth for visual values:
 * no component, material, or future effect may contain hard-coded numbers.
 *
 * Architectural decision: every token category is theme-aware, expressed as
 * a `Record<ThemeName, Category>`. This keeps the resolution logic generic
 * (one loop in `tokens/index.ts`) instead of duplicating per-category
 * theme-switching logic, and makes future themes a one-line addition.
 *
 * Token values are raw design values (CSS-ready strings for pixel sizes,
 * raw numbers for opacity), kept as strings/numbers so that future material
 * phases (blur, refraction, noise) can also consume them mathematically.
 */

import type { Elevation, ThemeName } from '../core/SurfaceTypes';

/** Corner radius scale. Strings are CSS-ready (e.g. "8px"). */
export interface RadiusTokens {
  xs: string;
  sm: string;
  md: string;
  lg: string;
  xl: string;
  full: string;
}

/** Border stroke widths. Strings are CSS-ready (e.g. "1px"). */
export interface BorderTokens {
  width: {
    hairline: string;
    thin: string;
    medium: string;
  };
}

/** Spacing scale for future components (padding, gaps, insets). */
export interface SpacingTokens {
  xs: string;
  sm: string;
  md: string;
  lg: string;
  xl: string;
  xxl: string;
}

/** Opacity scale used by materials and overlays. Values are 0..1. */
export interface OpacityTokens {
  surface: {
    solid: number;
    translucent: number;
    glass: number;
  };
  border: {
    hairline: number;
    default: number;
  };
  overlay: {
    subtle: number;
    scrim: number;
  };
  text: {
    primary: number;
    secondary: number;
    disabled: number;
  };
}

/**
 * Blur radii for future glass-family materials (Glass, Acrylic, Mica,
 * Liquid Glass). Phase 1 only defines them — no blur is rendered yet.
 * In `performance="low"` they are zeroed out by the resolver.
 */
export interface BlurTokens {
  sm: string;
  md: string;
  lg: string;
  xl: string;
}

/** Core color palette. Alpha is carried by the opacity tokens above. */
export interface ColorTokens {
  surface: {
    base: string;
    elevated: string;
    overlay: string;
  };
  text: {
    primary: string;
    secondary: string;
    disabled: string;
    inverse: string;
  };
  accent: {
    primary: string;
    hover: string;
    subtle: string;
  };
  border: {
    default: string;
    strong: string;
  };
  shadow: {
    base: string;
    strong: string;
  };
}

/**
 * One elevation step of the shadow scale. Numeric values (px) so future
 * phases can scale them per performance mode before serializing.
 */
export interface ShadowSpec {
  offsetX: number;
  offsetY: number;
  blur: number;
  spread: number;
  color: string;
}

/** Shadow scale keyed by elevation step (0..5). */
export type ShadowTokens = Record<Elevation, ShadowSpec>;

/** Motion tokens consumed by future animation phases (and components). */
export interface AnimationTokens {
  duration: {
    instant: string;
    fast: string;
    normal: string;
    slow: string;
    slower: string;
  };
  easing: {
    standard: string;
    decelerate: string;
    accelerate: string;
    spring: string;
  };
}

/**
 * The fully resolved, theme + performance aware token bundle handed to every
 * material renderer. Resolved once at the provider boundary — never per
 * component — so renderers stay pure and cheap.
 */
export interface ResolvedTokens {
  theme: ThemeName;
  colors: ColorTokens;
  opacity: OpacityTokens;
  blur: BlurTokens;
  radius: RadiusTokens;
  shadow: ShadowTokens;
  border: BorderTokens;
  spacing: SpacingTokens;
  animation: AnimationTokens;
}
