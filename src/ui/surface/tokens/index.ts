/**
 * tokens/index.ts
 *
 * Barrel for the token layer + the single token RESOLVER.
 *
 * Architectural decision: resolution happens exactly once per provider
 * state change, not per surface. The resolver picks the right record for
 * the active theme (guaranteed present by the Record<ThemeName, ...>
 * contract) and then applies a performance profile. Future adjustments
 * (e.g. `balanced` reducing shadow spread) slot into `applyPerformance`
 * without touching any component or material.
 */

import type { PerformanceMode, ThemeName } from '../core/SurfaceTypes';
import type { ResolvedTokens } from '../types/SurfaceToken';

import { blurTokens, zeroBlurTokens } from './blur';
import { borderTokens } from './border';
import { colorTokens } from './colors';
import { opacityTokens } from './opacity';
import { radiusTokens } from './radius';
import { shadowTokens } from './shadow';
import { spacingTokens } from './spacing';
import { animationTokens } from './animation';

export * from './animation';
export * from './blur';
export * from './border';
export * from './colors';
export * from './opacity';
export * from './radius';
export * from './shadow';
export * from './spacing';

/**
 * Resolves the full token bundle for a theme + performance mode.
 * This is the only place that composes the per-theme records together.
 */
export function resolveTokens(theme: ThemeName, performanceMode: PerformanceMode): ResolvedTokens {
  const resolved: ResolvedTokens = {
    theme,
    colors: colorTokens[theme],
    opacity: opacityTokens[theme],
    blur: blurTokens[theme],
    radius: radiusTokens[theme],
    shadow: shadowTokens[theme],
    border: borderTokens[theme],
    spacing: spacingTokens[theme],
    animation: animationTokens[theme],
  };

  return applyPerformance(resolved, performanceMode);
}

/**
 * Performance profiles degrade visual cost in one place.
 *
 * Phase 1: only `low` degrades (blur fully disabled — the cheapest glass
 * fallback). `balanced` and `high` share the full token set today;
 * future phases may scale shadow/blur per profile here without touching
 * any material.
 */
function applyPerformance(tokens: ResolvedTokens, mode: PerformanceMode): ResolvedTokens {
  if (mode === 'low') {
    return { ...tokens, blur: zeroBlurTokens };
  }
  return tokens;
}
