import type { Stats } from './types';

/** AASM adult PLMI threshold (events/hour) above which PLMs are flagged. */
export const PLMI_ABNORMAL_THRESHOLD = 15;

export interface Narrative {
  readonly headline: string;
  readonly lead: string;
  readonly verdict: string;
  readonly plmiAbnormal: boolean;
}

/**
 * Plain-language "your night at a glance" summary derived purely from the
 * scored stats — the calm-consumer narrative. No I/O; unit tested.
 */
export function buildNarrative(stats: Stats): Narrative {
  const plmiAbnormal = stats.plmi >= PLMI_ABNORMAL_THRESHOLD;
  const hrv = stats.hrv_rmssd_overall;
  const hrvPhrase =
    hrv === null ? 'Your heart-rate variability could not be measured this night.'
      : `Your heart rhythm stayed steady, with an average HRV of ${String(Math.round(hrv))} ms.`;

  if (plmiAbnormal) {
    return {
      headline: 'Your night at a glance',
      lead:
        `We noticed repeating leg movements through the night — small kicks that came in regular ` +
        `bursts and may have briefly stirred you without fully waking you. ${hrvPhrase}`,
      verdict: 'Above typical range',
      plmiAbnormal: true,
    };
  }

  if (stats.total_lms > 0) {
    return {
      headline: 'Your night at a glance',
      lead:
        `Your sleep looked mostly calm and steady. We saw ${String(stats.total_lms)} occasional leg ` +
        `movements, but not enough of a repeating pattern to flag. ${hrvPhrase}`,
      verdict: 'Within typical range',
      plmiAbnormal: false,
    };
  }

  return {
    headline: 'Your night at a glance',
    lead: `Your sleep looked calm and steady, with no notable leg movements detected. ${hrvPhrase}`,
    verdict: 'Within typical range',
    plmiAbnormal: false,
  };
}
