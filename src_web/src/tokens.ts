/**
 * Tremor-Illustrated design tokens (the subset the canvas charts need), kept in
 * lockstep with the CSS custom properties in styles.css. CSS owns the chrome,
 * this owns the ECharts canvas — both read the same palette.
 */
export const COLORS = {
  bg: '#0b1120',
  bg2: '#0e1628',
  surface: '#131c30',
  surface2: '#182236',
  ring: '#243049',
  ring2: '#2e3c59',
  grid: '#1c2740',

  text: '#e7ecf5',
  textMut: '#93a0bb',
  textDim: '#647193',

  cardiac: '#f0789a',
  cardiac2: '#ffb0c4',
  movement: '#5fd0c4',
  movement2: '#9af0e6',
  movement3: '#3fb8ad',
  neuro: '#8ea2ff',
  respir: '#f7b267',
  hrv: '#b388f5',

  good: '#4fd6a3',
  warn: '#f5b942',
  flag: '#ff7a7a',

  evtLm: '#4fd6a3',
  evtPlm: '#5fd0c4',
} as const;

export type ColorName = keyof typeof COLORS;
