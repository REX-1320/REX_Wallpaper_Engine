/**
 * SurfaceProvider.tsx
 *
 * The global provider — the single boundary where surface configuration is
 * declared and resolved into tokens.
 *
 * Responsibilities (Phase 1 spec):
 *   - current material
 *   - theme
 *   - performance mode
 *   - animation quality
 *   - surface defaults (elevation)
 *   - global updates (via the registry, see below)
 *
 * Architectural decisions:
 *
 * 1. Props are INITIAL VALUES, not live bindings. To change configuration
 *    at runtime, use the registry — the intended, imperative path for
 *    global updates:
 *
 *        surfaceRegistry.updateSurface('root', { theme: 'light' });
 *
 *    The provider subscribes to its own registry id and re-renders on
 *    changes. This keeps the provider a pure resolver and gives settings
 *    / personalization services a single global switch. Passing new props
 *    (or remounting) works too, but the registry is the canonical path.
 *
 * 2. Tokens are resolved HERE, once per state change, and memoized. Every
 *    descendant (and every material) receives the same resolved bundle —
 *    resolution never happens per-component.
 *
 * 3. The provider registers itself under `registryId` (default 'root')
 *    unless `register={false}`. It has no renderer of its own, so its
 *    registry entry carries none.
 */

import { useEffect, useMemo, useState, type ReactNode } from 'react';

import type {
  AnimationLevel,
  Elevation,
  MaterialType,
  PerformanceMode,
  ThemeName,
} from './SurfaceTypes';
import { SurfaceContext, type SurfaceContextValue } from './SurfaceContext';
import { surfaceRegistry } from './SurfaceRegistry';
import { resolveTokens } from '../tokens';

export interface SurfaceProviderProps {
  /** Initial global material. Default: 'solid'. */
  material?: MaterialType;
  /** Initial theme. Default: 'dark'. */
  theme?: ThemeName;
  /** Global performance profile. Default: 'balanced'. */
  performance?: PerformanceMode;
  /** Global animation quality. Default: 'full'. */
  animationLevel?: AnimationLevel;
  /** Default elevation for surfaces without an explicit one. Default: 0. */
  elevation?: Elevation;
  /**
   * Registry id used for global updates via `surfaceRegistry.updateSurface`.
   * Default: 'root'.
   */
  registryId?: string;
  /** Whether to register/subscribe to the registry. Default: true. */
  register?: boolean;
  children?: ReactNode;
}

interface ProviderState {
  material: MaterialType;
  theme: ThemeName;
  performanceMode: PerformanceMode;
  animationLevel: AnimationLevel;
  elevation: Elevation;
}

const DEFAULT_PROVIDER_STATE: ProviderState = {
  material: 'solid',
  theme: 'dark',
  performanceMode: 'balanced',
  animationLevel: 'full',
  elevation: 0,
};

export function SurfaceProvider({
  material = 'solid',
  theme = 'dark',
  performance = 'balanced',
  animationLevel = 'full',
  elevation = 0,
  registryId = 'root',
  register = true,
  children,
}: SurfaceProviderProps): ReactNode {
  const [state, setState] = useState<ProviderState>({
    material,
    theme,
    performanceMode: performance,
    animationLevel,
    elevation,
  });

  // Global updates: subscribe to the registry entry for this provider's id
  // and merge any patch (theme switches, material switches) into local state.
  // No local state is written for other providers' ids — each provider owns
  // its own id.
  useEffect(() => {
    if (!register) return;
    const unsubscribe = surfaceRegistry.subscribe(() => {
      const entry = surfaceRegistry.getSurface(registryId);
      if (entry === undefined) return;
      setState((prev) => ({
        ...prev,
        material: entry.material ?? prev.material,
        theme: entry.theme ?? prev.theme,
      }));
    });
    return unsubscribe;
  }, [registryId, register]);

  // Resolve tokens exactly once per state change, then memoize the entire
  // context value. `useMemo` on the state object identity keeps this cheap:
  // unrelated re-renders of the provider body (e.g. via children) do not
  // recompute anything.
  const value = useMemo<SurfaceContextValue>(() => {
    const tokens = resolveTokens(state.theme, state.performanceMode);
    return {
      material: state.material,
      theme: state.theme,
      performanceMode: state.performanceMode,
      animationLevel: state.animationLevel,
      elevation: state.elevation,
      tokens,
      colors: tokens.colors,
      blur: tokens.blur,
      opacity: tokens.opacity,
      radius: tokens.radius,
      shadow: tokens.shadow,
      border: tokens.border,
      spacing: tokens.spacing,
      animation: tokens.animation,
    };
  }, [state]);

  return <SurfaceContext.Provider value={value}>{children}</SurfaceContext.Provider>;
}
