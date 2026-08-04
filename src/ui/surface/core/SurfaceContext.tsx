/**
 * SurfaceContext.tsx
 *
 * The React context that carries the resolved surface configuration to
 * every component — no prop drilling, ever. Components read it through
 * `useSurface()` (see hooks/useSurface.ts).
 *
 * The context exposes exactly the Phase 1 contract:
 *   material, theme, blur, opacity, radius, shadow, border,
 *   elevation, animationLevel, performanceMode
 *
 * plus the full resolved `tokens` bundle. The category fields
 * (blur/opacity/radius/shadow/border) are NOT copies — they are direct
 * references into `tokens`, so there is exactly one source of truth and
 * zero duplicated logic; they exist purely as ergonomic accessors.
 *
 * A default value is provided so `useSurface()` never throws outside a
 * provider: unregistered components get the framework defaults
 * (solid / dark / balanced). Documented, intentional degradation.
 */

import { createContext } from 'react';

import type { ThemeName, MaterialType, PerformanceMode, AnimationLevel, Elevation } from './SurfaceTypes';
import type {
  AnimationTokens,
  BlurTokens,
  BorderTokens,
  ColorTokens,
  OpacityTokens,
  RadiusTokens,
  ResolvedTokens,
  ShadowTokens,
  SpacingTokens,
} from '../types/SurfaceToken';
import { resolveTokens } from '../tokens';

/** Everything a consumer may read from the surface system. */
export interface SurfaceContextValue {
  material: MaterialType;
  theme: ThemeName;
  performanceMode: PerformanceMode;
  animationLevel: AnimationLevel;
  elevation: Elevation;
  /** The complete resolved token bundle — the single source of truth. */
  tokens: ResolvedTokens;

  // Ergonomc accessors into `tokens` (same object references).
  colors: ColorTokens;
  blur: BlurTokens;
  opacity: OpacityTokens;
  radius: RadiusTokens;
  shadow: ShadowTokens;
  border: BorderTokens;
  spacing: SpacingTokens;
  animation: AnimationTokens;
}

/** The defaults a component sees when no provider exists above it. */
const DEFAULT_TOKENS = resolveTokens('dark', 'balanced');

export const DEFAULT_SURFACE_CONTEXT: SurfaceContextValue = {
  material: 'solid',
  theme: 'dark',
  performanceMode: 'balanced',
  animationLevel: 'full',
  elevation: 0,
  tokens: DEFAULT_TOKENS,
  colors: DEFAULT_TOKENS.colors,
  blur: DEFAULT_TOKENS.blur,
  opacity: DEFAULT_TOKENS.opacity,
  radius: DEFAULT_TOKENS.radius,
  shadow: DEFAULT_TOKENS.shadow,
  border: DEFAULT_TOKENS.border,
  spacing: DEFAULT_TOKENS.spacing,
  animation: DEFAULT_TOKENS.animation,
};

export const SurfaceContext = createContext<SurfaceContextValue>(DEFAULT_SURFACE_CONTEXT);
