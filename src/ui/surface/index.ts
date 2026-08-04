/**
 * surface/index.ts
 *
 * Public API of the REX Surface System (Phase 1 — architecture only).
 *
 * Everything a consumer needs comes from this single barrel:
 *
 *     import { Surface, SurfaceProvider, useSurface } from 'ui/surface';
 *     import type { SurfaceVariant, ThemeName } from 'ui/surface';
 *
 * Layer pipeline implemented by this package:
 *
 *     Component → Surface → Material → Theme Tokens → Renderer
 */

export * from './core';
export * from './materials';
export * from './hooks/useSurface';
export * from './tokens';
export * from './types/SurfaceToken';
