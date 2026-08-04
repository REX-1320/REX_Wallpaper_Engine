/**
 * colors.ts
 *
 * Core color palette, defined per theme. Structural tokens (radius, spacing,
 * border width) are theme-independent; colors are exactly what changes when
 * the theme switches, so this is where the light/dark/highContrast
 * divergence lives.
 *
 * Alpha is NOT baked into these hex values — it is applied through the
 * opacity tokens so glass materials can reuse the same hue at different
 * opacities (the future liquid-glass tint strategy).
 */

import type { ThemeName } from '../core/SurfaceTypes';
import type { ColorTokens } from '../types/SurfaceToken';

const LIGHT: ColorTokens = {
  surface: {
    base: '#F6F6F6',
    elevated: '#FFFFFF',
    overlay: '#FAFAFA',
  },
  text: {
    primary: '#1A1A1A',
    secondary: '#4A4A4A',
    disabled: '#8A8A8A',
    inverse: '#FFFFFF',
  },
  accent: {
    primary: '#3B82F6',
    hover: '#2563EB',
    subtle: '#DBEAFE',
  },
  border: {
    default: '#1A1A1A',
    strong: '#0D0D0D',
  },
  shadow: {
    base: '#1A1A1A',
    strong: '#000000',
  },
};

const DARK: ColorTokens = {
  surface: {
    base: '#1F1F1F',
    elevated: '#2A2A2A',
    overlay: '#242424',
  },
  text: {
    primary: '#F5F5F5',
    secondary: '#B5B5B5',
    disabled: '#6E6E6E',
    inverse: '#1A1A1A',
  },
  accent: {
    primary: '#4C9AFF',
    hover: '#6FB0FF',
    subtle: '#14304F',
  },
  border: {
    default: '#F5F5F5',
    strong: '#FFFFFF',
  },
  shadow: {
    base: '#000000',
    strong: '#000000',
  },
};

export const colorTokens: Record<ThemeName, ColorTokens> = {
  light: LIGHT,
  dark: DARK,
  highContrast: {
    surface: { base: '#000000', elevated: '#000000', overlay: '#000000' },
    text: { primary: '#FFFFFF', secondary: '#FFFFFF', disabled: '#9E9E9E', inverse: '#000000' },
    accent: { primary: '#FFFF00', hover: '#FFFF00', subtle: '#000000' },
    border: { default: '#FFFFFF', strong: '#FFFFFF' },
    shadow: { base: '#000000', strong: '#000000' },
  },
};
