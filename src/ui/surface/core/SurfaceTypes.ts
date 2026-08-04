/**
 * SurfaceTypes.ts
 *
 * The vocabulary of the Surface System — every noun (material, variant,
 * theme, performance mode, elevation, state) is defined here, exactly once.
 *
 * Architectural decision: the system has TWO concepts that must not be
 * conflated:
 *
 *   1. MATERIALS  — physical renderers: solid, glass, acrylic, mica,
 *                   liquidGlass, transparent. A material answers
 *                   "HOW is this surface rendered?".
 *   2. VARIANTS   — semantic intents: overlay, card, elevated (plus every
 *                   material). A variant answers "WHAT is this surface
 *                   FOR?" and is resolved into a material + elevation
 *                   preset (see VARIANT_DEFAULTS).
 *
 * Components should declare VARIANTS (what they are), never materials
 * (how they look). When Liquid Glass ships, swapping the card preset to
 * `material: 'liquidGlass'` restyles the entire app without touching a
 * single component file.
 */

import type { ComponentType, CSSProperties, ElementType, ReactNode } from 'react';
import type { ResolvedTokens } from '../types/SurfaceToken';

/** The six physical renderers. Extended here when new materials land. */
export const MATERIALS = ['solid', 'glass', 'acrylic', 'mica', 'liquidGlass', 'transparent'] as const;

export type MaterialType = (typeof MATERIALS)[number];

/**
 * Every surface intent: the six materials plus the three semantic variants
 * (Overlay, Card, Elevated). Defined per the Phase 1 spec.
 */
export const SURFACE_VARIANTS = [...MATERIALS, 'overlay', 'card', 'elevated'] as const;

export type SurfaceVariant = (typeof SURFACE_VARIANTS)[number];

/** Light / dark / high-contrast. 'highContrast' is accessibility-first. */
export type ThemeName = 'light' | 'dark' | 'highContrast';

/** Cost profiles: 'low' disables blur, 'balanced' is the default. */
export type PerformanceMode = 'low' | 'balanced' | 'high';

/**
 * Motion preference level — respects the OS reduced-motion setting.
 * Components check this before running any animation.
 */
export type AnimationLevel = 'full' | 'reduced' | 'none';

/** Depth scale used to index the shadow tokens. */
export type Elevation = 0 | 1 | 2 | 3 | 4 | 5;

/** Lifecycle state tracked by the surface registry. */
export type SurfaceState = 'active' | 'inactive';

/** What a semantic variant resolves to. */
export interface VariantDefaults {
  material: MaterialType;
  elevation: Elevation;
}

/**
 * Variant → (material, elevation) presets.
 *
 * Phase 1: card/overlay/elevated deliberately resolve to `solid` with
 * increasing elevation — the ONLY renderable material today. Future
 * phases will retarget these presets (e.g. card → liquidGlass) without
 * any component change. This is the entire point of the framework.
 */
export const VARIANT_DEFAULTS: Record<SurfaceVariant, VariantDefaults> = {
  solid: { material: 'solid', elevation: 0 },
  glass: { material: 'glass', elevation: 0 },
  acrylic: { material: 'acrylic', elevation: 0 },
  mica: { material: 'mica', elevation: 0 },
  liquidGlass: { material: 'liquidGlass', elevation: 0 },
  transparent: { material: 'transparent', elevation: 0 },
  card: { material: 'solid', elevation: 1 },
  overlay: { material: 'solid', elevation: 3 },
  elevated: { material: 'solid', elevation: 4 },
};

/** Narrowing guard: is this variant one of the six physical materials? */
export function isMaterial(variant: SurfaceVariant): variant is MaterialType {
  return (MATERIALS as readonly string[]).includes(variant);
}

/**
 * Props every material renderer receives. Deliberately small: materials are
 * pure — they take the pre-resolved tokens + elevation and render. They do
 * NOT know about themes, performance modes, or context.
 */
export interface MaterialProps {
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
  /** Theme + performance resolved token bundle (see ResolvedTokens). */
  tokens: ResolvedTokens;
  /** The shadow/elevation step this surface sits at. */
  elevation: Elevation;
}

/** A material renderer is just a component that accepts MaterialProps. */
export type SurfaceRenderer = ComponentType<MaterialProps>;

/**
 * The element a Surface may render as (div, section, ...). Defaults to a
 * plain div; overridable per-surface for semantic HTML.
 */
export type SurfaceElementType = ElementType;
