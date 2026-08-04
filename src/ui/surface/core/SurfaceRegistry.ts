/**
 * SurfaceRegistry.ts
 *
 * A framework-level, React-free store of every known surface definition.
 *
 * Architectural decision: the registry deliberately has ZERO React
 * dependencies. It is the imperative channel for GLOBAL updates (theme
 * switching, material hot-swapping, enabling/disabling surfaces) that
 * must work outside the render tree and from any module (settings,
 * personalization service, command palette). React components subscribe
 * to it (see SurfaceProvider) instead of owning global state.
 *
 * Each entry carries:
 *   id       – unique identifier (built-ins use their material name,
 *              app-level surfaces use their own names)
 *   material – the material type it renders with
 *   theme    – the theme this surface was defined/updated for
 *   state    – active | inactive (registry-level lifecycle)
 *   renderer – the material renderer component (optional for
 *              provider-only registrations)
 *
 * Custom surfaces register with `registerSurface` — no factory changes
 * required. That is the extension point for third-party materials.
 */

import type { MaterialType, SurfaceRenderer, SurfaceState, ThemeName } from './SurfaceTypes';
import { MATERIALS } from './SurfaceTypes';

/** A registered surface definition. */
export interface SurfaceRegistration {
  id: string;
  material: MaterialType;
  theme: ThemeName;
  state: SurfaceState;
  renderer?: SurfaceRenderer;
}

/** Partial update applied by `updateSurface` — only provided keys change. */
export type SurfaceUpdate = Partial<Pick<SurfaceRegistration, 'material' | 'theme' | 'state' | 'renderer'>>;

/** Listener invoked after any registry mutation. */
export type SurfaceRegistryListener = () => void;

export interface SurfaceRegistry {
  /** Registers a surface. Throws if the id is already registered. */
  registerSurface(entry: SurfaceRegistration): void;
  /** Removes a surface. Returns true if it existed. */
  unregisterSurface(id: string): boolean;
  /** Applies a partial update. Returns true if the surface existed. */
  updateSurface(id: string, patch: SurfaceUpdate): boolean;
  /** Looks up a surface by id. */
  getSurface(id: string): SurfaceRegistration | undefined;
  /** All registered surfaces. */
  getAllSurfaces(): readonly SurfaceRegistration[];
  /** Subscribes to mutations. Returns an unsubscribe function. */
  subscribe(listener: SurfaceRegistryListener): () => void;
}

/** Guards a material value at runtime — protects the registry from bad input. */
function assertKnownMaterial(material: MaterialType): void {
  if (!(MATERIALS as readonly string[]).includes(material)) {
    throw new Error(`SurfaceRegistry: unknown material "${material}".`);
  }
}

/** Factory: creates a fresh registry (testable in isolation). */
export function createSurfaceRegistry(): SurfaceRegistry {
  const surfaces = new Map<string, SurfaceRegistration>();
  const listeners = new Set<SurfaceRegistryListener>();

  function notify(): void {
    for (const listener of listeners) {
      listener();
    }
  }

  return {
    registerSurface(entry: SurfaceRegistration): void {
      assertKnownMaterial(entry.material);
      if (surfaces.has(entry.id)) {
        throw new Error(`SurfaceRegistry: surface "${entry.id}" is already registered.`);
      }
      surfaces.set(entry.id, entry);
      notify();
    },

    unregisterSurface(id: string): boolean {
      const removed = surfaces.delete(id);
      if (removed) notify();
      return removed;
    },

    updateSurface(id: string, patch: SurfaceUpdate): boolean {
      const existing = surfaces.get(id);
      if (!existing) return false;
      if (patch.material !== undefined) assertKnownMaterial(patch.material);
      surfaces.set(id, { ...existing, ...patch });
      notify();
      return true;
    },

    getSurface(id: string): SurfaceRegistration | undefined {
      return surfaces.get(id);
    },

    getAllSurfaces(): readonly SurfaceRegistration[] {
      return [...surfaces.values()];
    },

    subscribe(listener: SurfaceRegistryListener): () => void {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}

/**
 * The app-wide singleton registry. Imported directly by the provider,
 * factory, and any imperative code that needs to switch themes globally.
 */
export const surfaceRegistry = createSurfaceRegistry();
