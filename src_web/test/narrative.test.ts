import { describe, it, expect } from 'vitest';
import { buildNarrative, PLMI_ABNORMAL_THRESHOLD } from '../src/narrative';
import type { Stats } from '../src/types';

const baseStats = (over: Partial<Stats>): Stats => ({
  plmi: 0,
  total_lms: 0,
  total_plms: 0,
  total_hours: 6,
  hrv_rmssd_overall: 42,
  threshold: 8,
  window_sec: 30,
  fs: 10,
  ...over,
});

describe('buildNarrative', () => {
  it('flags PLMI at or above the AASM threshold', () => {
    const n = buildNarrative(baseStats({ plmi: PLMI_ABNORMAL_THRESHOLD, total_lms: 142, total_plms: 88 }));
    expect(n.plmiAbnormal).toBe(true);
    expect(n.verdict).toBe('Above typical range');
    expect(n.lead).toContain('repeating leg movements');
  });

  it('treats occasional LMs below threshold as within range', () => {
    const n = buildNarrative(baseStats({ plmi: 3, total_lms: 38 }));
    expect(n.plmiAbnormal).toBe(false);
    expect(n.verdict).toBe('Within typical range');
    expect(n.lead).toContain('38 occasional leg movements');
  });

  it('reads as calm when no LMs were detected', () => {
    const n = buildNarrative(baseStats({ total_lms: 0 }));
    expect(n.lead).toContain('no notable leg movements');
  });

  it('handles a missing HRV measurement', () => {
    const n = buildNarrative(baseStats({ hrv_rmssd_overall: null }));
    expect(n.lead).toContain('could not be measured');
  });
});
